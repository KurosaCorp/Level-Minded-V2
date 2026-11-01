#include "editor/TransformGizmo.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;

namespace lm {

namespace {

constexpr float kGizmoPixels = 100.0f;  // axis length on screen at 100% DPI
constexpr float kHitPixels = 8.0f;      // how close the mouse must be to a handle
constexpr float kCenterPixels = 8.0f;   // radius of the center handle
constexpr float kPlaneNear = 0.24f;     // plane handle square, as fractions of the axis length
constexpr float kPlaneFar = 0.44f;
constexpr float kArrowBase = 0.80f;     // where the arrow head starts along the axis
constexpr float kAxisStart = 0.18f;     // axis hit zone starts here (the center handle owns the middle)
constexpr float kScreenRing = 1.22f;    // outer rotate ring radius, relative to the axis rings
constexpr int kRingSegments = 64;

constexpr float kSnapMove = 0.25f;                          // meters
constexpr float kSnapAngle = XMConvertToRadians(15.0f);
constexpr float kSnapScale = 0.1f;
constexpr float kMinFactor = 0.01f;

constexpr ImU32 kAxisColors[3] = {IM_COL32(230, 62, 70, 255), IM_COL32(108, 200, 52, 255), IM_COL32(66, 133, 244, 255)};
constexpr ImU32 kHighlight = IM_COL32(255, 214, 64, 255);
constexpr ImU32 kCenterColor = IM_COL32(235, 235, 235, 255);

ImU32 WithAlpha(ImU32 color, int alpha)
{
    return (color & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
}

float Dot(FXMVECTOR a, FXMVECTOR b)
{
    return XMVectorGetX(XMVector3Dot(a, b));
}

// World point -> screen pixels. False if the point is behind the camera.
bool Project(const GizmoView& view, FXMVECTOR world, ImVec2& out)
{
    const XMVECTOR clip = XMVector4Transform(XMVectorSetW(world, 1.0f), XMLoadFloat4x4(&view.viewProj));
    const float w = XMVectorGetW(clip);
    if (w <= 1e-5f)
        return false;
    out.x = view.rect.x + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * view.rect.width;
    out.y = view.rect.y + (0.5f - XMVectorGetY(clip) / w * 0.5f) * view.rect.height;
    return true;
}

float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
{
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float apx = p.x - a.x, apy = p.y - a.y;
    const float lengthSq = abx * abx + aby * aby;
    const float t = lengthSq > 0.0f ? std::clamp((apx * abx + apy * aby) / lengthSq, 0.0f, 1.0f) : 0.0f;
    return std::hypot(apx - abx * t, apy - aby * t);
}

// Convex quad, either winding: inside if on the same side of all four edges.
bool PointInQuad(ImVec2 p, const ImVec2 (&quad)[4])
{
    int positive = 0, negative = 0;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 a = quad[i], b = quad[(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        positive += cross > 0.0f;
        negative += cross < 0.0f;
    }
    return positive == 0 || negative == 0;
}

// Parameter s of the point on the line origin + s * axis (axis unit length) closest to the ray.
// False if the ray is (nearly) parallel to the line.
bool ClosestOnAxis(FXMVECTOR origin, FXMVECTOR axis, const Ray& ray, float& s)
{
    const XMVECTOR d = XMLoadFloat3(&ray.direction);
    const XMVECTOR w = XMVectorSubtract(origin, XMLoadFloat3(&ray.origin));
    const float b = Dot(axis, d);
    const float denominator = 1.0f - b * b;
    if (denominator < 1e-5f)
        return false;
    s = (b * Dot(d, w) - Dot(axis, w)) / denominator;
    return true;
}

bool IntersectPlane(const Ray& ray, FXMVECTOR point, FXMVECTOR normal, XMVECTOR& hit)
{
    const XMVECTOR d = XMLoadFloat3(&ray.direction);
    const XMVECTOR o = XMLoadFloat3(&ray.origin);
    const float dn = Dot(d, normal);
    if (std::abs(dn) < 1e-6f)
        return false;
    const float t = Dot(XMVectorSubtract(point, o), normal) / dn;
    if (t < 0.0f)
        return false;
    hit = XMVectorMultiplyAdd(d, XMVectorReplicate(t), o);
    return true;
}

float SignedAngle(FXMVECTOR from, FXMVECTOR to, FXMVECTOR axis)
{
    return std::atan2(Dot(XMVector3Cross(from, to), axis), Dot(from, to));
}

float SnapTo(float value, float step)
{
    return std::round(value / step) * step;
}

} // namespace

int TransformGizmo::AxisOf(Handle handle)
{
    switch (handle) {
    case Handle::AxisX: case Handle::PlaneX: return 0;
    case Handle::AxisY: case Handle::PlaneY: return 1;
    case Handle::AxisZ: case Handle::PlaneZ: return 2;
    case Handle::None: case Handle::Center: break;
    }
    return -1;
}

namespace {

bool IsAxisHandle(int handle, int axisX)
{
    return handle >= axisX && handle < axisX + 3;
}

} // namespace

void TransformGizmo::SetMode(GizmoMode mode)
{
    if (IsDragging())
        return;
    m_mode = mode;
    m_hovered = Handle::None;
}

void TransformGizmo::Refresh(const SceneTree& scene, NodeId primary)
{
    m_targetsVersion = scene.RenderVersion();
    m_targetsTransformVersion = scene.TransformVersion();
    m_primaryRequested = primary;
    m_targets.clear(); // keeps capacity: no allocation unless the selection grew
    if (scene.SelectedItemCount() == 0)
        return;
    m_targets.reserve(scene.SelectedItemCount()); // at most one allocation, when the selection grows

    XMVECTOR sum = XMVectorZero();
    for (uint32_t i = 0; i < scene.Size();) {
        const SceneTree::Node& node = scene.NodeAt(i);
        if (!node.Has(SceneTree::kEffectivelyVisible)) {
            i = scene.SubtreeEnd(i);
            continue;
        }
        if (!node.IsGroup() && node.Has(SceneTree::kSelected)) {
            const Transform& t = scene.TransformAt(i);
            m_targets.push_back(node.id);
            sum = XMVectorAdd(sum, XMLoadFloat3(&t.position));
            if (m_targets.size() == 1 || node.id == primary)
                m_primaryRotation = t.rotation; // the primary item, or else the first one
        }
        ++i;
    }
    if (!m_targets.empty())
        XMStoreFloat3(&m_pivot, XMVectorScale(sum, 1.0f / float(m_targets.size())));
}

XMVECTOR TransformGizmo::CurrentFrame() const
{
    const bool local = m_space == GizmoSpace::Local || (m_mode == GizmoMode::Scale && m_targets.size() == 1);
    return local ? XMLoadFloat4(&m_primaryRotation) : XMQuaternionIdentity();
}

void TransformGizmo::Update(SceneTree& scene, NodeId primary, const GizmoView& view, bool allowHover, bool snap)
{
    if (!IsDragging() && (scene.RenderVersion() != m_targetsVersion || scene.TransformVersion() != m_targetsTransformVersion ||
                          primary != m_primaryRequested))
        Refresh(scene, primary);

    m_hovered = Handle::None;
    if (m_targets.empty()) {
        m_visible = false;
        m_active = Handle::None;
        return;
    }
    if (IsDragging())
        ApplyDrag(scene, view, snap);
    UpdateGeometry(view);
    if (!IsDragging() && allowHover && m_visible)
        m_hovered = HitTest(view);
}

void TransformGizmo::UpdateGeometry(const GizmoView& view)
{
    const XMVECTOR toPivot = XMVectorSubtract(XMLoadFloat3(&m_pivot), XMLoadFloat3(&view.eye));
    const float depth = Dot(toPivot, XMLoadFloat3(&view.forward));
    m_visible = depth > view.nearZ * 2.0f && !view.rect.IsEmpty();
    if (!m_visible)
        return;

    // Constant size on screen: world size of kGizmoPixels at the pivot's depth.
    m_length = kGizmoPixels * view.dpiScale * view.worldPerPixelAtUnitDepth * depth;
    XMStoreFloat3(&m_viewDir, XMVector3Normalize(toPivot));
    const XMVECTOR frame = IsDragging() ? XMLoadFloat4(&m_dragFrame) : CurrentFrame();
    XMStoreFloat3(&m_axes[0], XMVector3Rotate(g_XMIdentityR0, frame));
    XMStoreFloat3(&m_axes[1], XMVector3Rotate(g_XMIdentityR1, frame));
    XMStoreFloat3(&m_axes[2], XMVector3Rotate(g_XMIdentityR2, frame));
}

bool TransformGizmo::AxisVisible(int axis) const
{
    // An axis pointing (almost) at the camera can't be dragged reliably.
    return std::abs(Dot(XMLoadFloat3(&m_axes[axis]), XMLoadFloat3(&m_viewDir))) < 0.985f;
}

bool TransformGizmo::PlaneVisible(int axis) const
{
    // Hide planes seen edge-on.
    return std::abs(Dot(XMLoadFloat3(&m_axes[axis]), XMLoadFloat3(&m_viewDir))) > 0.2f;
}

TransformGizmo::Handle TransformGizmo::HitTest(const GizmoView& view)
{
    const ImVec2 mouse(view.mouse.x, view.mouse.y);
    const XMVECTOR pivot = XMLoadFloat3(&m_pivot);
    ImVec2 center;
    if (!Project(view, pivot, center))
        return Handle::None;
    const float hitDistance = kHitPixels * view.dpiScale;
    const float centerDistance = std::hypot(mouse.x - center.x, mouse.y - center.y);
    const XMVECTOR length = XMVectorReplicate(m_length);

    float best = hitDistance;
    Handle bestHandle = Handle::None;

    if (m_mode == GizmoMode::Rotate) {
        // Rings: only the half facing the camera is clickable.
        const XMVECTOR toEye = XMVectorNegate(XMLoadFloat3(&m_viewDir));
        for (int axis = 0; axis < 3; ++axis) {
            const XMVECTOR u = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 1) % 3]), length);
            const XMVECTOR v = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 2) % 3]), length);
            XMVECTOR previousWorld = XMVectorAdd(pivot, u);
            ImVec2 previous;
            bool previousValid = Project(view, previousWorld, previous);
            for (int k = 1; k <= kRingSegments; ++k) {
                const float angle = XM_2PI * float(k) / float(kRingSegments);
                const XMVECTOR world = XMVectorAdd(pivot, XMVectorAdd(XMVectorScale(u, std::cos(angle)), XMVectorScale(v, std::sin(angle))));
                ImVec2 point;
                const bool valid = Project(view, world, point);
                if (valid && previousValid) {
                    const XMVECTOR middle = XMVectorSubtract(XMVectorScale(XMVectorAdd(world, previousWorld), 0.5f), pivot);
                    if (Dot(XMVector3Normalize(middle), toEye) >= -0.05f) {
                        const float distance = DistanceToSegment(mouse, previous, point);
                        if (distance < best) {
                            best = distance;
                            bestHandle = static_cast<Handle>(int(Handle::AxisX) + axis);
                            XMStoreFloat3(&m_hoverGrab, XMVectorAdd(pivot, middle));
                        }
                    }
                }
                previous = point;
                previousValid = valid;
                previousWorld = world;
            }
        }
        if (bestHandle != Handle::None)
            return bestHandle;
        if (std::abs(centerDistance - kGizmoPixels * kScreenRing * view.dpiScale) < hitDistance)
            return Handle::Center;
        return Handle::None;
    }

    // Move / scale: center first, then axes, then (move only) planes.
    if (centerDistance <= kCenterPixels * view.dpiScale)
        return Handle::Center;
    const float axisEnd = m_mode == GizmoMode::Scale ? 1.06f : 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (!AxisVisible(axis))
            continue;
        const XMVECTOR direction = XMVectorMultiply(XMLoadFloat3(&m_axes[axis]), length);
        ImVec2 a, b;
        if (!Project(view, XMVectorMultiplyAdd(direction, XMVectorReplicate(kAxisStart), pivot), a) ||
            !Project(view, XMVectorMultiplyAdd(direction, XMVectorReplicate(axisEnd), pivot), b))
            continue;
        const float distance = DistanceToSegment(mouse, a, b);
        if (distance < best) {
            best = distance;
            bestHandle = static_cast<Handle>(int(Handle::AxisX) + axis);
        }
    }
    if (bestHandle != Handle::None || m_mode != GizmoMode::Translate)
        return bestHandle;

    for (int axis = 0; axis < 3; ++axis) {
        if (!PlaneVisible(axis))
            continue;
        const XMVECTOR u = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 1) % 3]), length);
        const XMVECTOR v = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 2) % 3]), length);
        const float s[4][2] = {{kPlaneNear, kPlaneNear}, {kPlaneFar, kPlaneNear}, {kPlaneFar, kPlaneFar}, {kPlaneNear, kPlaneFar}};
        ImVec2 quad[4];
        bool valid = true;
        for (int c = 0; c < 4 && valid; ++c)
            valid = Project(view, XMVectorAdd(pivot, XMVectorAdd(XMVectorScale(u, s[c][0]), XMVectorScale(v, s[c][1]))), quad[c]);
        if (valid && PointInQuad(mouse, quad))
            return static_cast<Handle>(int(Handle::PlaneX) + axis);
    }
    return Handle::None;
}

bool TransformGizmo::BeginDrag(const SceneTree& scene, const GizmoView& view)
{
    if (m_hovered == Handle::None || !m_visible)
        return false;

    const Handle handle = m_hovered;
    const XMVECTOR pivot = XMLoadFloat3(&m_pivot);
    const Ray& ray = view.mouseRay;
    const int axisIndex = AxisOf(handle);
    const bool axisHandle = IsAxisHandle(int(handle), int(Handle::AxisX));
    // Axis, plane normal, or (center handles) the view direction.
    const XMVECTOR axis = axisIndex >= 0 ? XMLoadFloat3(&m_axes[axisIndex]) : XMLoadFloat3(&view.forward);

    m_dragUsesTangent = false;
    m_dragAngle = 0.0f;
    m_dragShownAngle = 0.0f;
    m_dragFactor = {1.0f, 1.0f, 1.0f};
    m_label[0] = '\0';

    switch (m_mode) {
    case GizmoMode::Translate:
        if (axisHandle) {
            if (!ClosestOnAxis(pivot, axis, ray, m_dragStartParam))
                return false;
        } else {
            XMVECTOR hit;
            if (!IntersectPlane(ray, pivot, axis, hit))
                return false;
            XMStoreFloat3(&m_dragStartHit, hit);
        }
        break;

    case GizmoMode::Rotate: {
        // Normally the angle is measured in the ring's plane. A ring seen nearly edge-on makes that
        // unstable, so then the mouse drags along the ring's tangent on screen instead.
        XMVECTOR hit;
        XMVECTOR start;
        if (std::abs(Dot(axis, XMLoadFloat3(&m_viewDir))) >= 0.25f && IntersectPlane(ray, pivot, axis, hit)) {
            start = XMVectorSubtract(hit, pivot);
            XMStoreFloat3(&m_dragPrevVector, start);
        } else {
            if (handle == Handle::Center)
                return false;
            m_dragUsesTangent = true;
            const XMVECTOR grab = XMLoadFloat3(&m_hoverGrab);
            start = XMVectorSubtract(grab, pivot);
            const XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(axis, start));
            ImVec2 a, b;
            if (!Project(view, grab, a) || !Project(view, XMVectorMultiplyAdd(tangent, XMVectorReplicate(m_length * 0.1f), grab), b))
                return false;
            const float length = std::hypot(b.x - a.x, b.y - a.y);
            if (length < 1e-3f)
                return false;
            m_dragTangent = {(b.x - a.x) / length, (b.y - a.y) / length};
        }
        // Remember where the drag started (unit vector in the ring plane) for the swept-angle display.
        start = XMVectorSubtract(start, XMVectorScale(axis, Dot(start, axis)));
        if (XMVectorGetX(XMVector3LengthSq(start)) < 1e-12f)
            return false;
        XMStoreFloat3(&m_dragStartHit, XMVector3Normalize(start));
        break;
    }

    case GizmoMode::Scale:
        if (axisHandle) {
            if (!ClosestOnAxis(pivot, axis, ray, m_dragStartParam))
                return false;
            m_dragStartParam = std::max(m_dragStartParam, 0.25f * m_length); // grabbing near the center
        }
        break;
    }

    m_active = handle;
    XMStoreFloat3(&m_dragAxis, axis);
    XMStoreFloat4(&m_dragFrame, CurrentFrame());
    m_dragPivot = m_pivot;
    m_dragLength = m_length;
    m_dragStartMouse = view.mouse;
    m_startTransforms.resize(m_targets.size());
    for (size_t k = 0; k < m_targets.size(); ++k) {
        const uint32_t index = scene.IndexOf(m_targets[k]);
        m_startTransforms[k] = index != SceneTree::kNoIndex ? scene.TransformAt(index) : Transform{};
    }
    return true;
}

void TransformGizmo::ApplyDrag(SceneTree& scene, const GizmoView& view, bool snap)
{
    const Ray& ray = view.mouseRay;
    const XMVECTOR pivot = XMLoadFloat3(&m_dragPivot);
    const XMVECTOR axis = XMLoadFloat3(&m_dragAxis);
    const int axisIndex = AxisOf(m_active);
    const bool axisHandle = IsAxisHandle(int(m_active), int(Handle::AxisX));
    const auto applyAll = [&](auto&& transform) {
        for (size_t k = 0; k < m_targets.size(); ++k)
            scene.SetTransform(m_targets[k], transform(m_startTransforms[k]));
    };

    switch (m_mode) {
    case GizmoMode::Translate: {
        XMVECTOR delta;
        if (axisHandle) {
            float s;
            if (!ClosestOnAxis(pivot, axis, ray, s))
                return;
            float distance = s - m_dragStartParam;
            if (snap)
                distance = SnapTo(distance, kSnapMove);
            delta = XMVectorScale(axis, distance);
        } else {
            XMVECTOR hit;
            if (!IntersectPlane(ray, pivot, axis, hit))
                return;
            delta = XMVectorSubtract(hit, XMLoadFloat3(&m_dragStartHit));
            if (XMVectorGetX(XMVector3Length(delta)) > 1.0e4f * m_dragLength)
                return; // ray nearly parallel to the plane: the hit point ran off to the horizon
            if (snap) { // snap in the gizmo's axes (world axes for the free-move center)
                const XMVECTOR frame = m_active == Handle::Center ? XMQuaternionIdentity() : XMLoadFloat4(&m_dragFrame);
                XMVECTOR local = XMVector3Rotate(delta, XMQuaternionInverse(frame));
                local = XMVectorScale(XMVectorRound(XMVectorScale(local, 1.0f / kSnapMove)), kSnapMove);
                delta = XMVector3Rotate(local, frame);
            }
        }
        applyAll([&](const Transform& t) { return Translated(t, delta); });
        XMStoreFloat3(&m_pivot, XMVectorAdd(pivot, delta));
        XMFLOAT3 d;
        XMStoreFloat3(&d, delta);
        std::snprintf(m_label, sizeof(m_label), "%+.2f  %+.2f  %+.2f m", d.x, d.y, d.z);
        break;
    }

    case GizmoMode::Rotate: {
        if (m_dragUsesTangent) {
            const float moved = (view.mouse.x - m_dragStartMouse.x) * m_dragTangent.x + (view.mouse.y - m_dragStartMouse.y) * m_dragTangent.y;
            m_dragAngle = moved / (kGizmoPixels * view.dpiScale); // one radian per gizmo radius
        } else {
            XMVECTOR hit;
            if (IntersectPlane(ray, pivot, axis, hit)) {
                const XMVECTOR v = XMVectorSubtract(hit, pivot);
                if (XMVectorGetX(XMVector3LengthSq(v)) > 1e-8f * m_dragLength * m_dragLength) {
                    // Accumulate small steps so the angle can go past +-180 degrees.
                    m_dragAngle += SignedAngle(XMLoadFloat3(&m_dragPrevVector), v, axis);
                    XMStoreFloat3(&m_dragPrevVector, v);
                }
            }
        }
        const float angle = snap ? SnapTo(m_dragAngle, kSnapAngle) : m_dragAngle;
        const XMVECTOR rotation = XMQuaternionRotationNormal(axis, angle);
        applyAll([&](const Transform& t) { return RotatedAbout(t, pivot, rotation); });
        m_dragShownAngle = angle;
        std::snprintf(m_label, sizeof(m_label), "%+.1f\xC2\xB0", XMConvertToDegrees(angle));
        break;
    }

    case GizmoMode::Scale: {
        XMFLOAT3 factor{1.0f, 1.0f, 1.0f};
        if (axisHandle) {
            float s;
            if (!ClosestOnAxis(pivot, axis, ray, s))
                return;
            float f = std::max(s / m_dragStartParam, kMinFactor);
            if (snap)
                f = std::max(SnapTo(f, kSnapScale), kSnapScale);
            (&factor.x)[axisIndex] = f;
            std::snprintf(m_label, sizeof(m_label), "%c x%.2f", "XYZ"[axisIndex], f);
        } else {
            const float moved = (view.mouse.x - m_dragStartMouse.x) - (view.mouse.y - m_dragStartMouse.y);
            float f = std::max(1.0f + moved / (kGizmoPixels * view.dpiScale), kMinFactor);
            if (snap)
                f = std::max(SnapTo(f, kSnapScale), kSnapScale);
            factor = {f, f, f};
            std::snprintf(m_label, sizeof(m_label), "x%.2f", f);
        }
        m_dragFactor = factor;
        const XMVECTOR frame = XMLoadFloat4(&m_dragFrame);
        const XMVECTOR factorVector = XMLoadFloat3(&factor);
        applyAll([&](const Transform& t) { return ScaledAbout(t, pivot, frame, factorVector); });
        break;
    }
    }
}

std::unique_ptr<Command> TransformGizmo::EndDrag(const SceneTree& scene)
{
    if (!IsDragging())
        return nullptr;
    m_active = Handle::None;
    std::vector<NodeId> nodes;
    std::vector<Transform> before, after;
    for (size_t k = 0; k < m_targets.size() && k < m_startTransforms.size(); ++k) {
        const uint32_t index = scene.IndexOf(m_targets[k]);
        if (index == SceneTree::kNoIndex || scene.TransformAt(index) == m_startTransforms[k])
            continue;
        nodes.push_back(m_targets[k]);
        before.push_back(m_startTransforms[k]);
        after.push_back(scene.TransformAt(index));
    }
    if (nodes.empty())
        return nullptr; // clicked a handle without moving it
    const char* name = m_mode == GizmoMode::Translate ? "Move" : m_mode == GizmoMode::Rotate ? "Rotate" : "Scale";
    return std::make_unique<TransformCommand>(name, std::move(nodes), std::move(before), std::move(after));
}

void TransformGizmo::CancelDrag(SceneTree& scene)
{
    if (!IsDragging())
        return;
    for (size_t k = 0; k < m_targets.size(); ++k)
        scene.SetTransform(m_targets[k], m_startTransforms[k]);
    m_pivot = m_dragPivot;
    m_active = Handle::None;
}

void TransformGizmo::Draw(ImDrawList* drawList, const GizmoView& view) const
{
    if (!m_visible || m_targets.empty())
        return;
    const XMVECTOR pivot = XMLoadFloat3(&m_pivot);
    ImVec2 center;
    if (!Project(view, pivot, center))
        return;

    const float dpi = view.dpiScale;
    const float thickness = 2.5f * dpi;
    const bool dragging = IsDragging();
    const Handle focus = dragging ? m_active : m_hovered;
    const auto colorFor = [&](Handle handle, ImU32 base) {
        if (handle == focus)
            return kHighlight;
        return dragging ? WithAlpha(base, 70) : base; // other handles step back while dragging
    };
    const auto axisHandle = [](int axis) { return static_cast<Handle>(int(Handle::AxisX) + axis); };
    const XMVECTOR length = XMVectorReplicate(m_length);

    drawList->PushClipRect(ImVec2(view.rect.x, view.rect.y), ImVec2(view.rect.x + view.rect.width, view.rect.y + view.rect.height), true);

    switch (m_mode) {
    case GizmoMode::Translate: {
        for (int axis = 0; axis < 3; ++axis) {
            if (!PlaneVisible(axis))
                continue;
            const XMVECTOR u = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 1) % 3]), length);
            const XMVECTOR v = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 2) % 3]), length);
            const float s[4][2] = {{kPlaneNear, kPlaneNear}, {kPlaneFar, kPlaneNear}, {kPlaneFar, kPlaneFar}, {kPlaneNear, kPlaneFar}};
            ImVec2 q[4];
            bool valid = true;
            for (int c = 0; c < 4 && valid; ++c)
                valid = Project(view, XMVectorAdd(pivot, XMVectorAdd(XMVectorScale(u, s[c][0]), XMVectorScale(v, s[c][1]))), q[c]);
            if (!valid)
                continue;
            const Handle handle = static_cast<Handle>(int(Handle::PlaneX) + axis);
            const ImU32 color = colorFor(handle, kAxisColors[axis]);
            drawList->AddQuadFilled(q[0], q[1], q[2], q[3], WithAlpha(color, handle == focus ? 120 : dragging ? 25 : 60));
            drawList->AddQuad(q[0], q[1], q[2], q[3], WithAlpha(color, dragging && handle != focus ? 70 : 210), 1.5f * dpi);
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (!AxisVisible(axis))
                continue;
            const XMVECTOR direction = XMVectorMultiply(XMLoadFloat3(&m_axes[axis]), length);
            ImVec2 base, tip;
            if (!Project(view, XMVectorMultiplyAdd(direction, XMVectorReplicate(kArrowBase), pivot), base) ||
                !Project(view, XMVectorAdd(pivot, direction), tip))
                continue;
            const ImU32 color = colorFor(axisHandle(axis), kAxisColors[axis]);
            drawList->AddLine(center, base, color, thickness);
            const float dx = tip.x - base.x, dy = tip.y - base.y;
            const float headLength = std::hypot(dx, dy);
            if (headLength > 1.0f) {
                const float w = 5.5f * dpi / headLength;
                drawList->AddTriangleFilled(tip, ImVec2(base.x - dy * w, base.y + dx * w), ImVec2(base.x + dy * w, base.y - dx * w), color);
            }
        }
        const ImU32 color = colorFor(Handle::Center, kCenterColor);
        if (focus == Handle::Center)
            drawList->AddCircleFilled(center, kCenterPixels * 0.8f * dpi, WithAlpha(color, 140));
        drawList->AddCircle(center, kCenterPixels * 0.8f * dpi, color, 0, 2.0f * dpi);
        break;
    }

    case GizmoMode::Rotate: {
        const XMVECTOR toEye = XMVectorNegate(XMLoadFloat3(&m_viewDir));
        for (int axis = 0; axis < 3; ++axis) {
            const XMVECTOR u = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 1) % 3]), length);
            const XMVECTOR v = XMVectorMultiply(XMLoadFloat3(&m_axes[(axis + 2) % 3]), length);
            const ImU32 color = colorFor(axisHandle(axis), kAxisColors[axis]);
            XMVECTOR previousWorld = XMVectorAdd(pivot, u);
            ImVec2 previous;
            bool previousValid = Project(view, previousWorld, previous);
            for (int k = 1; k <= kRingSegments; ++k) {
                const float angle = XM_2PI * float(k) / float(kRingSegments);
                const XMVECTOR world = XMVectorAdd(pivot, XMVectorAdd(XMVectorScale(u, std::cos(angle)), XMVectorScale(v, std::sin(angle))));
                ImVec2 point;
                const bool valid = Project(view, world, point);
                if (valid && previousValid) {
                    const XMVECTOR middle = XMVectorSubtract(XMVectorScale(XMVectorAdd(world, previousWorld), 0.5f), pivot);
                    if (Dot(XMVector3Normalize(middle), toEye) >= -0.05f)
                        drawList->AddLine(previous, point, color, thickness);
                    else
                        drawList->AddLine(previous, point, WithAlpha(color, 50), 1.2f * dpi); // back half
                }
                previous = point;
                previousValid = valid;
                previousWorld = world;
            }
        }
        drawList->AddCircle(center, kGizmoPixels * kScreenRing * dpi, colorFor(Handle::Center, IM_COL32(220, 220, 220, 190)), 64, 1.5f * dpi);

        // Swept angle as a pie in the ring's plane.
        if (dragging && std::abs(m_dragShownAngle) > 1e-4f) {
            const XMVECTOR axis = XMLoadFloat3(&m_dragAxis);
            const float radius = m_length * (m_active == Handle::Center ? kScreenRing : 1.0f);
            const XMVECTOR start = XMVectorScale(XMLoadFloat3(&m_dragStartHit), radius);
            const int segments = std::clamp(int(std::abs(m_dragShownAngle) / XM_2PI * 64.0f) + 2, 2, 256);
            ImVec2 previous;
            bool previousValid = Project(view, XMVectorAdd(pivot, start), previous);
            if (previousValid)
                drawList->AddLine(center, previous, WithAlpha(kHighlight, 160), 1.5f * dpi);
            for (int k = 1; k <= segments; ++k) {
                const XMVECTOR q = XMQuaternionRotationNormal(axis, m_dragShownAngle * float(k) / float(segments));
                ImVec2 point;
                const bool valid = Project(view, XMVectorAdd(pivot, XMVector3Rotate(start, q)), point);
                if (valid && previousValid)
                    drawList->AddTriangleFilled(center, previous, point, WithAlpha(kHighlight, 55));
                if (valid && k == segments)
                    drawList->AddLine(center, point, WithAlpha(kHighlight, 220), 1.5f * dpi);
                previous = point;
                previousValid = valid;
            }
        }
        break;
    }

    case GizmoMode::Scale: {
        const float box = 4.5f * dpi;
        for (int axis = 0; axis < 3; ++axis) {
            if (!AxisVisible(axis))
                continue;
            const float stretch = dragging ? (&m_dragFactor.x)[axis] : 1.0f; // the handle follows the factor
            ImVec2 end;
            if (!Project(view, XMVectorMultiplyAdd(XMLoadFloat3(&m_axes[axis]), XMVectorReplicate(m_length * stretch), pivot), end))
                continue;
            const ImU32 color = colorFor(axisHandle(axis), kAxisColors[axis]);
            drawList->AddLine(center, end, color, thickness);
            drawList->AddRectFilled(ImVec2(end.x - box, end.y - box), ImVec2(end.x + box, end.y + box), color);
        }
        const float half = kCenterPixels * 0.75f * dpi;
        drawList->AddRectFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                                colorFor(Handle::Center, kCenterColor));
        break;
    }
    }

    // Live value next to the cursor while dragging.
    if (dragging && m_label[0] != '\0') {
        const ImVec2 position(view.mouse.x + 18.0f * dpi, view.mouse.y + 14.0f * dpi);
        const ImVec2 size = ImGui::CalcTextSize(m_label);
        const float pad = 4.0f * dpi;
        drawList->AddRectFilled(ImVec2(position.x - pad, position.y - pad), ImVec2(position.x + size.x + pad, position.y + size.y + pad),
                                IM_COL32(20, 20, 24, 215), 4.0f * dpi);
        drawList->AddText(position, IM_COL32(240, 240, 240, 255), m_label);
    }
    drawList->PopClipRect();
}

} // namespace lm
