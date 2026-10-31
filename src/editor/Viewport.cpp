#include "editor/Viewport.h"

#include "core/Clock.h"
#include "editor/NodeStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace DirectX;

namespace lm {

namespace {

constexpr float kOrbitRadiansPerPixel = 0.0050f;
constexpr float kLookRadiansPerPixel = 0.0030f;
constexpr float kDollyPerPixel = 0.0060f;  // log-distance per pixel of Alt+RMB drag
constexpr float kDollyPerWheelStep = 0.18f;
constexpr float kFlyFastMultiplier = 4.0f;
constexpr float kFlySlowMultiplier = 0.25f;
constexpr float kMaxFrameDelta = 0.1f;     // seconds; avoids jumps after an idle period

constexpr float kHalfPi = XM_PIDIV2;

// The Z axis is retained scene geometry, so it has a fixed length (well past any grid fade distance).
constexpr float kAxisLength = 100000.0f;
constexpr uint32_t kAxisZColor = PackColor(66, 133, 244);
constexpr uint32_t kAxisZBelowColor = PackColor(66, 133, 244, 80);

} // namespace

void Viewport::BeginDrag(Drag drag, int mouseButton)
{
    m_drag = drag;
    m_dragButton = mouseButton;
}

ViewportEvents Viewport::Update(const Rect& rect, float dpiScale, SceneTree& scene, NodeId activeNode)
{
    ViewportEvents events;
    m_rect = rect;
    DrawToolbar(dpiScale);

    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = !io.WantCaptureMouse && rect.Contains(io.MousePos.x, io.MousePos.y);

    // Keyboard focus: any click in the viewport (or its toolbar) takes it, any click on other UI gives it back.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_hasKeyboardFocus = hovered || m_toolbarHovered;
        if (hovered)
            ImGui::SetWindowFocus(nullptr);
    }

    if (m_drag == Drag::None && hovered && !m_gizmo.IsDragging()) {
        if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            BeginDrag(Drag::Orbit, ImGuiMouseButton_Left);
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            BeginDrag(Drag::Pan, ImGuiMouseButton_Middle);
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            BeginDrag(io.KeyAlt ? Drag::Dolly : Drag::Fly, ImGuiMouseButton_Right);
    }
    if (m_drag != Drag::None && !ImGui::IsMouseDown(m_dragButton))
        m_drag = Drag::None;

    // Mouse deltas are in physical pixels; normalize rotation speed across DPI scales.
    const float dx = io.MouseDelta.x;
    const float dy = io.MouseDelta.y;
    const float invDpi = 1.0f / std::max(dpiScale, 0.5f);

    switch (m_drag) {
    case Drag::Orbit:
        m_camera.Orbit(-dx * kOrbitRadiansPerPixel * invDpi, dy * kOrbitRadiansPerPixel * invDpi);
        break;
    case Drag::Pan:
        m_camera.Pan(dx, dy, rect.height);
        break;
    case Drag::Dolly:
        m_camera.Dolly((dx - dy) * kDollyPerPixel * invDpi);
        break;
    case Drag::Fly: {
        m_camera.Look(-dx * kLookRadiansPerPixel * invDpi, dy * kLookRadiansPerPixel * invDpi);
        const auto axis = [](ImGuiKey positive, ImGuiKey negative) {
            return (ImGui::IsKeyDown(positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(negative) ? 1.0f : 0.0f);
        };
        const XMFLOAT3 direction{axis(ImGuiKey_D, ImGuiKey_A), axis(ImGuiKey_E, ImGuiKey_Q), axis(ImGuiKey_W, ImGuiKey_S)};
        float speed = m_flySpeed;
        if (io.KeyShift)
            speed *= kFlyFastMultiplier;
        if (io.KeyCtrl)
            speed *= kFlySlowMultiplier;
        m_camera.Move(direction, speed * std::min(io.DeltaTime, kMaxFrameDelta));
        if (io.MouseWheel != 0.0f)
            m_flySpeed = std::clamp(m_flySpeed * std::pow(1.25f, io.MouseWheel), 0.1f, 5000.0f);
        break;
    }
    case Drag::None:
        if (hovered && io.MouseWheel != 0.0f)
            m_camera.Dolly(io.MouseWheel * kDollyPerWheelStep);
        break;
    }

    // Gizmo and click selection, against the camera as it is drawn this frame.
    m_gizmoView = MakeGizmoView(dpiScale);
    m_gizmo.Update(scene, activeNode, m_gizmoView, hovered && m_drag == Drag::None, io.KeyCtrl);
    if (m_gizmo.IsDragging() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_gizmo.EndDrag();
    if (m_drag == Drag::None && hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_gizmo.IsDragging() && !m_gizmo.BeginDrag(scene, m_gizmoView))
        events.picked = PickAndSelect(scene, m_gizmoView.mouseRay, io.KeyCtrl, io.KeyShift);

    if (m_drag != Drag::Fly)
        HandleShortcuts(scene);
    return events;
}

void Viewport::FrameSelection(const SceneTree& scene)
{
    if (!m_gizmo.HasTargets()) {
        m_camera.SetTarget({0.0f, 0.0f, 0.0f});
        return;
    }
    // Bounding box of everything selected, then fit its bounding sphere in the view.
    XMVECTOR lo = XMVectorReplicate(INFINITY), hi = XMVectorReplicate(-INFINITY);
    for (const NodeId id : m_gizmo.Targets()) {
        const uint32_t index = scene.IndexOf(id);
        if (index == SceneTree::kNoIndex)
            continue;
        const Bounds bounds = scene.WorldBoundsAt(index);
        const XMVECTOR center = XMLoadFloat3(&bounds.center), half = XMLoadFloat3(&bounds.halfExtents);
        lo = XMVectorMin(lo, XMVectorSubtract(center, half));
        hi = XMVectorMax(hi, XMVectorAdd(center, half));
    }
    if (XMVector3IsInfinite(lo)) // no target still exists
        return;
    XMFLOAT3 target;
    XMStoreFloat3(&target, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    m_camera.SetTarget(target);
    m_camera.FrameSphere(std::max(XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) * 0.5f, 0.05f));
}

NodeId Viewport::PickAndSelect(SceneTree& scene, const Ray& ray, bool toggle, bool add)
{
    const double start = Clock::Now();
    const PickHit hit = m_meshes ? PickItem(scene, *m_meshes, ray) : PickHit{};
    m_lastPickMs = (Clock::Now() - start) * 1000.0;
    m_lastPickItems = hit.itemsTested;

    if (!hit) {
        if (!toggle && !add)
            scene.ClearSelection(); // click on empty space
        return kInvalidNode;
    }
    const bool wasSelected = scene.NodeAt(scene.IndexOf(hit.node)).Has(SceneTree::kSelected);
    if (toggle) {
        scene.SetSelected(hit.node, !wasSelected);
    } else {
        if (!add)
            scene.ClearSelection();
        scene.SetSelected(hit.node, true);
    }
    return hit.node;
}

GizmoView Viewport::MakeGizmoView(float dpiScale) const
{
    const ImGuiIO& io = ImGui::GetIO();
    GizmoView view;
    view.rect = m_rect;
    XMStoreFloat4x4(&view.viewProj, XMMatrixMultiply(m_camera.ViewMatrix(), m_camera.ProjectionMatrix(m_rect.Aspect())));
    view.eye = m_camera.Eye();
    XMStoreFloat3(&view.forward, m_camera.ForwardVector());
    view.worldPerPixelAtUnitDepth = 2.0f * std::tan(m_camera.FovY() * 0.5f) / std::max(m_rect.height, 1.0f);
    view.nearZ = m_camera.NearZ();
    view.mouse = {io.MousePos.x, io.MousePos.y};
    const float ndcX = m_rect.width > 0.0f ? (io.MousePos.x - m_rect.x) / m_rect.width * 2.0f - 1.0f : 0.0f;
    const float ndcY = m_rect.height > 0.0f ? 1.0f - (io.MousePos.y - m_rect.y) / m_rect.height * 2.0f : 0.0f;
    view.mouseRay = m_camera.ScreenRay(ndcX, ndcY, m_rect.Aspect());
    view.dpiScale = dpiScale;
    return view;
}

void Viewport::DrawToolbar(float dpiScale)
{
    m_toolbarHovered = false;
    if (m_rect.IsEmpty())
        return;

    ImGui::SetNextWindowPos(ImVec2(m_rect.x + m_rect.width * 0.5f, m_rect.y + 8.0f * dpiScale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##ViewportToolbar", nullptr, kFlags)) {
        const auto modeButton = [&](const char* label, GizmoMode mode, const char* tooltip) {
            const bool active = m_gizmo.Mode() == mode;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label))
                m_gizmo.SetMode(mode);
            if (active)
                ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tooltip);
            ImGui::SameLine();
        };
        modeButton("Move", GizmoMode::Translate, "Move (W)\nDrag an arrow, a plane square or the center circle.\nHold Ctrl to snap to 0.25 m.");
        modeButton("Rotate", GizmoMode::Rotate, "Rotate (E)\nDrag a ring, or the outer ring to turn around the view.\nHold Ctrl to snap to 15 degrees.");
        modeButton("Scale", GizmoMode::Scale, "Scale (R)\nDrag an axis box, or the center box to scale evenly.\nHold Ctrl to snap to 0.1 steps.");
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool local = m_gizmo.Space() == GizmoSpace::Local;
        const float width = ImGui::CalcTextSize("World").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::Button(local ? "Local###space" : "World###space", ImVec2(width, 0.0f)))
            m_gizmo.SetSpace(local ? GizmoSpace::World : GizmoSpace::Local);
        ImGui::SetItemTooltip("Gizmo axes (X): world axes, or the selected item's own axes.\n"
                              "Scaling a single item always uses its own axes.");
        // AllowWhenBlockedByActiveItem: on the frame a button is pressed, the window still counts as hovered.
        m_toolbarHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    ImGui::End();
}

void Viewport::HandleShortcuts(SceneTree& scene)
{
    // ImGui's own WantCaptureKeyboard is not used here: with keyboard navigation enabled it stays set
    // whenever any UI window (even the dockspace host) has nav focus.
    const ImGuiIO& io = ImGui::GetIO();
    if (!m_hasKeyboardFocus || io.WantTextInput)
        return;

    if (m_gizmo.IsDragging()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            m_gizmo.CancelDrag(scene);
        return;
    }

    if (!io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            m_gizmo.SetMode(GizmoMode::Translate);
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            m_gizmo.SetMode(GizmoMode::Rotate);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            m_gizmo.SetMode(GizmoMode::Scale);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            m_gizmo.SetSpace(m_gizmo.Space() == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        FrameSelection(scene);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
        m_camera.Reset();

    const bool opposite = io.KeyCtrl;
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad1, false)) // front: looking along +Y
        m_camera.SetOrientation(opposite ? kHalfPi : -kHalfPi, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad3, false)) // right: looking along -X
        m_camera.SetOrientation(opposite ? XM_PI : 0.0f, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_Keypad7, false)) // top: looking along -Z
        m_camera.SetOrientation(-kHalfPi, opposite ? -kHalfPi : kHalfPi);
}

const char* Viewport::InteractionName() const
{
    switch (m_drag) {
    case Drag::Orbit: return "Orbit";
    case Drag::Pan:   return "Pan";
    case Drag::Dolly: return "Dolly";
    case Drag::Fly:   return "Fly";
    case Drag::None:  break;
    }
    return "-";
}

ViewportCamera Viewport::RenderCamera() const
{
    ViewportCamera camera{};
    XMStoreFloat4x4(&camera.view, m_camera.ViewMatrix());
    XMStoreFloat4x4(&camera.projection, m_camera.ProjectionMatrix(m_rect.Aspect()));
    camera.eye = m_camera.Eye();
    // Show more of the plane the higher / farther the camera is.
    camera.gridFadeDistance = std::max({std::abs(camera.eye.z) * 60.0f, m_camera.Distance() * 8.0f, 60.0f});
    return camera;
}

bool Viewport::SubmitScene(const SceneTree& scene, ViewportRenderer& renderer)
{
    if (scene.RenderVersion() == m_submittedRenderVersion && m_drawSettings.showAxes == m_submittedShowAxes &&
        m_transformChangesApplied <= scene.TransformChanges().size()) {
        if (scene.TransformVersion() != m_submittedTransformVersion)
            PatchMovedItems(scene, renderer); // same items, some moved
        return false;
    }
    m_submittedRenderVersion = scene.RenderVersion();
    m_submittedTransformVersion = scene.TransformVersion();
    m_transformChangesApplied = scene.TransformChanges().size(); // already part of this rebuild
    m_submittedShowAxes = m_drawSettings.showAxes;
    m_submitted.assign(scene.IdCapacity(), SubmittedItem{});

    renderer.BeginScene();
    if (m_drawSettings.showAxes) { // vertical Z axis (X and Y are drawn by the grid shader)
        renderer.SceneLines().AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, kAxisLength}, kAxisZColor);
        renderer.SceneLines().AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -kAxisLength}, kAxisZBelowColor);
    }

    // One linear pass over the node array; hidden subtrees are skipped in one jump.
    for (uint32_t i = 0; i < scene.Size();) {
        const SceneTree::Node& node = scene.NodeAt(i);
        if (!node.Has(SceneTree::kEffectivelyVisible)) {
            i = scene.SubtreeEnd(i);
            continue;
        }
        if (!node.IsGroup()) {
            const ItemData& item = scene.ItemAt(i);
            const Transform& t = item.transform;
            const XMFLOAT3 size{t.scale.x * item.halfExtents.x, t.scale.y * item.halfExtents.y, t.scale.z * item.halfExtents.z};
            const bool selected = node.Has(SceneTree::kSelected);
            SubmittedItem& submitted = m_submitted[node.id];
            if (item.mesh != MeshId::None) {
                uint32_t flags = selected ? kMeshFlagSelected : 0u;
                if (node.kind == NodeKind::Light)
                    flags |= kMeshFlagEmissive;
                // Imported models carry their material colors in the vertices; built-ins use the kind's color.
                const uint32_t color = IsImportedMesh(item.mesh) ? PackColor(255, 255, 255) : KindAlbedo(node.kind);
                submitted.mesh = renderer.AddMeshInstance(item.mesh, t.position, t.rotation, size, color, flags);
            } else if (!selected) {
                submitted.lines = renderer.SceneLines().AddBox(ItemMatrix(t, item.halfExtents), KindColor(node.kind)); // volume
            }
            if (selected)
                submitted.outline = renderer.AddOutline(t.position, t.rotation, size, kSelectionColor);
        }
        ++i;
    }
    renderer.EndScene();
    return true;
}

void Viewport::PatchMovedItems(const SceneTree& scene, ViewportRenderer& renderer)
{
    const std::span<const NodeId> changes = scene.TransformChanges();
    for (size_t k = m_transformChangesApplied; k < changes.size(); ++k) {
        const NodeId id = changes[k];
        const uint32_t index = scene.IndexOf(id);
        if (id >= m_submitted.size() || index == SceneTree::kNoIndex)
            continue;
        const SubmittedItem& submitted = m_submitted[id];
        const ItemData& item = scene.ItemAt(index);
        const Transform& t = item.transform;
        const XMFLOAT3 size{t.scale.x * item.halfExtents.x, t.scale.y * item.halfExtents.y, t.scale.z * item.halfExtents.z};
        if (submitted.mesh != kNoInstance)
            renderer.UpdateInstance(submitted.mesh, t.position, t.rotation, size);
        if (submitted.outline != kNoInstance)
            renderer.UpdateInstance(submitted.outline, t.position, t.rotation, size);
        if (submitted.lines != kNoInstance) {
            renderer.SceneLines().SetBox(submitted.lines, ItemMatrix(t, item.halfExtents));
            renderer.SceneLinesChanged();
        }
    }
    m_transformChangesApplied = changes.size();
    m_submittedTransformVersion = scene.TransformVersion();
}

void Viewport::DrawOverlays(ImDrawList* drawList, float dpiScale) const
{
    if (m_rect.IsEmpty())
        return;
    m_gizmo.Draw(drawList, m_gizmoView);

    const float radius = 34.0f * dpiScale;
    const float margin = 16.0f * dpiScale;
    const ImVec2 center(m_rect.x + m_rect.width - radius - margin, m_rect.y + radius + margin);

    struct AxisMark {
        XMFLOAT3 direction;
        ImU32 color;
        const char* label;
        float depth = 0.0f;
        ImVec2 screen{};
    };
    AxisMark marks[] = {
        {{1, 0, 0}, IM_COL32(230, 62, 70, 255), "X"},  {{0, 1, 0}, IM_COL32(108, 200, 52, 255), "Y"},
        {{0, 0, 1}, IM_COL32(66, 133, 244, 255), "Z"}, {{-1, 0, 0}, IM_COL32(230, 62, 70, 110), nullptr},
        {{0, -1, 0}, IM_COL32(108, 200, 52, 110), nullptr}, {{0, 0, -1}, IM_COL32(66, 133, 244, 110), nullptr},
    };

    const XMMATRIX view = m_camera.ViewMatrix();
    for (AxisMark& mark : marks) {
        XMFLOAT3 v;
        XMStoreFloat3(&v, XMVector3TransformNormal(XMLoadFloat3(&mark.direction), view));
        mark.screen = ImVec2(center.x + v.x * radius, center.y - v.y * radius);
        mark.depth = v.z; // view space looks down -Z: larger z = closer to the viewer
    }
    std::sort(std::begin(marks), std::end(marks), [](const AxisMark& a, const AxisMark& b) { return a.depth < b.depth; });

    drawList->AddCircleFilled(center, radius + 10.0f * dpiScale, IM_COL32(255, 255, 255, 14));
    const float bubble = 8.0f * dpiScale;
    for (const AxisMark& mark : marks) {
        if (mark.label) {
            drawList->AddLine(center, mark.screen, mark.color, 2.0f * dpiScale);
            drawList->AddCircleFilled(mark.screen, bubble, mark.color);
            const ImVec2 textSize = ImGui::CalcTextSize(mark.label);
            drawList->AddText(ImVec2(mark.screen.x - textSize.x * 0.5f, mark.screen.y - textSize.y * 0.5f),
                              IM_COL32(20, 20, 24, 255), mark.label);
        } else {
            drawList->AddCircleFilled(mark.screen, bubble * 0.75f, mark.color);
        }
    }
}

} // namespace lm
