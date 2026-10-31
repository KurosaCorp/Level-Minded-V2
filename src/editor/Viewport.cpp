#include "editor/Viewport.h"

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

} // namespace

void Viewport::BeginDrag(Drag drag, int mouseButton)
{
    m_drag = drag;
    m_dragButton = mouseButton;
}

void Viewport::Update(const Rect& rect, float dpiScale)
{
    m_rect = rect;
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = !io.WantCaptureMouse && rect.Contains(io.MousePos.x, io.MousePos.y);

    // Keyboard focus: any click in the viewport takes it, any click on UI gives it back.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_hasKeyboardFocus = hovered;
        if (hovered)
            ImGui::SetWindowFocus(nullptr);
    }

    if (m_drag == Drag::None && hovered) {
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

    if (m_drag != Drag::Fly)
        HandleShortcuts();
}

void Viewport::HandleShortcuts()
{
    // ImGui's own WantCaptureKeyboard is not used here: with keyboard navigation enabled it stays set
    // whenever any UI window (even the dockspace host) has nav focus.
    const ImGuiIO& io = ImGui::GetIO();
    if (!m_hasKeyboardFocus || io.WantTextInput)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        m_camera.SetTarget({0.0f, 0.0f, 0.0f});
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

void Viewport::DrawOrientationGizmo(ImDrawList* drawList, float dpiScale) const
{
    if (m_rect.IsEmpty())
        return;

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
