#pragma once

#include "core/Rect.h"
#include "scene/Commands.h"
#include "scene/Picking.h"
#include "scene/SceneTree.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct ImDrawList;

namespace lm {

enum class GizmoMode : uint8_t { Translate, Rotate, Scale };
enum class GizmoSpace : uint8_t { World, Local };

// Camera and mouse state for one frame, shared by hit testing, dragging and drawing.
struct GizmoView {
    Rect rect;                         // viewport in screen pixels
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3 eye;
    DirectX::XMFLOAT3 forward;         // unit view direction
    float worldPerPixelAtUnitDepth = 0.0f; // 2 tan(fovY / 2) / viewport height
    float nearZ = 0.05f;
    Ray mouseRay;
    DirectX::XMFLOAT2 mouse;           // screen pixels
    float dpiScale = 1.0f;
};

// Move / rotate / scale handles for the selected items, drawn with ImGui into the viewport.
//
// Targets are the selected, effectively visible items (selecting a group transforms everything in
// it). The gizmo sits at their average position and keeps a constant size on screen. Its axes are
// the world axes, or in Local space the primary item's own axes (scale on a single item always uses
// the item's axes, since a rotated box can't be stretched along world axes without shearing).
//
// Handles:  Move    - axis arrows, plane squares, center circle (moves in the view plane)
//           Rotate  - one ring per axis, outer ring (around the view direction)
//           Scale   - axis boxes, center box (uniform)
// Every drag recomputes the transforms from the values captured at drag start, so long drags do
// not accumulate error, and Esc restores them. Hold Ctrl to snap (0.25 m, 15 degrees, 0.1x).
class TransformGizmo {
public:
    GizmoMode Mode() const { return m_mode; }
    void SetMode(GizmoMode mode);
    GizmoSpace Space() const { return m_space; }
    void SetSpace(GizmoSpace space) { m_space = space; }

    // Once per frame, after the camera has moved. Refreshes the targets when the scene changed,
    // applies an ongoing drag, then hit-tests the handles under the mouse if `allowHover`.
    void Update(SceneTree& scene, NodeId primary, const GizmoView& view, bool allowHover, bool snap);
    // Starts dragging the hovered handle. Returns false (and does nothing) if no handle is hovered.
    bool BeginDrag(const SceneTree& scene, const GizmoView& view);
    // Finishes the drag. Returns the undo step for it (a TransformCommand with every target's
    // before/after transform), or null if nothing actually changed.
    std::unique_ptr<Command> EndDrag(const SceneTree& scene);
    void CancelDrag(SceneTree& scene); // puts every target back where the drag started

    void Draw(ImDrawList* drawList, const GizmoView& view) const;

    bool IsHovered() const { return m_hovered != Handle::None; }
    bool IsDragging() const { return m_active != Handle::None; }
    bool HasTargets() const { return !m_targets.empty(); }
    uint32_t TargetCount() const { return static_cast<uint32_t>(m_targets.size()); }
    const DirectX::XMFLOAT3& Pivot() const { return m_pivot; }
    std::span<const NodeId> Targets() const { return m_targets; }

private:
    enum class Handle : uint8_t { None, AxisX, AxisY, AxisZ, PlaneX, PlaneY, PlaneZ, Center };

    static int AxisOf(Handle handle);  // 0..2 for Axis*/Plane*, -1 otherwise
    void Refresh(const SceneTree& scene, NodeId primary);
    DirectX::XMVECTOR CurrentFrame() const;
    void UpdateGeometry(const GizmoView& view);
    Handle HitTest(const GizmoView& view);
    void ApplyDrag(SceneTree& scene, const GizmoView& view, bool snap);
    bool AxisVisible(int axis) const;
    bool PlaneVisible(int axis) const;

    GizmoMode m_mode = GizmoMode::Translate;
    GizmoSpace m_space = GizmoSpace::World;
    Handle m_hovered = Handle::None;
    Handle m_active = Handle::None;

    // Targets: rebuilt only when the scene's render/transform version or the primary node changes.
    std::vector<NodeId> m_targets;
    std::vector<Transform> m_startTransforms; // captured by BeginDrag, parallel to m_targets
    uint64_t m_targetsVersion = ~0ull;
    uint64_t m_targetsTransformVersion = ~0ull;
    NodeId m_primaryRequested = kInvalidNode;
    DirectX::XMFLOAT4 m_primaryRotation{0.0f, 0.0f, 0.0f, 1.0f};

    // Geometry of this frame.
    DirectX::XMFLOAT3 m_pivot{};
    DirectX::XMFLOAT3 m_axes[3]{};
    DirectX::XMFLOAT3 m_viewDir{};   // eye -> pivot, unit
    float m_length = 1.0f;           // axis length, world units
    bool m_visible = false;
    DirectX::XMFLOAT3 m_hoverGrab{}; // ring point under the mouse (rotate)

    // Drag state, captured by BeginDrag.
    DirectX::XMFLOAT3 m_dragPivot{};
    DirectX::XMFLOAT4 m_dragFrame{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 m_dragAxis{};    // axis, plane normal or rotation axis
    DirectX::XMFLOAT3 m_dragStartHit{};
    DirectX::XMFLOAT3 m_dragPrevVector{};
    DirectX::XMFLOAT2 m_dragStartMouse{};
    DirectX::XMFLOAT2 m_dragTangent{}; // screen direction of positive rotation (tangent drag)
    float m_dragLength = 1.0f;
    float m_dragStartParam = 0.0f;
    float m_dragAngle = 0.0f;          // accumulated, radians
    bool m_dragUsesTangent = false;
    DirectX::XMFLOAT3 m_dragFactor{1.0f, 1.0f, 1.0f}; // scale shown while dragging
    float m_dragShownAngle = 0.0f;
    char m_label[64] = {};
};

} // namespace lm
