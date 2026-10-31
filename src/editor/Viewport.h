#pragma once

#include "core/Rect.h"
#include "editor/EditorCamera.h"
#include "editor/TransformGizmo.h"
#include "render/ViewportRenderer.h"
#include "scene/Picking.h"
#include "scene/SceneTree.h"

#include <vector>

struct ImDrawList;

namespace lm {

// What happened in the viewport this frame that other panels react to.
struct ViewportEvents {
    NodeId picked = kInvalidNode; // item clicked in the view (the scene selection is already updated)
};

// The central 3D view: owns the editor camera, the transform gizmo and click selection.
//
// Controls (only when the cursor is over the viewport and not over UI):
//   LMB click             select the item under the cursor (Ctrl: toggle, Shift: add; empty space clears)
//   LMB drag on gizmo     move / rotate / scale the selection (Ctrl snaps, Esc cancels)
//   W / E / R             move / rotate / scale gizmo      X   world / local axes
//   Alt + LMB drag        orbit            MMB drag (or Alt + MMB)   pan
//   Alt + RMB drag        dolly            Mouse wheel               dolly
//   RMB drag              fly-look; WASD move, Q/E down/up, Shift fast, Ctrl slow, wheel = fly speed
//   F                     frame the selection (origin if none) Home   reset view
//   Numpad 1 / 3 / 7      front / right / top view (Ctrl: back / left / bottom)
class Viewport {
public:
    // The meshes items refer to (used for click picking). Must be set before the first Update.
    void SetMeshLibrary(const MeshLibrary* meshes) { m_meshes = meshes; }

    // Processes input for this frame. Call between ImGui::NewFrame() and ImGui::Render().
    // `activeNode` is the hierarchy's active node; the gizmo uses its axes in Local space.
    ViewportEvents Update(const Rect& rect, float dpiScale, SceneTree& scene, NodeId activeNode);
    // Orientation axes (top-right corner) and the transform gizmo.
    void DrawOverlays(ImDrawList* drawList, float dpiScale) const;

    ViewportCamera RenderCamera() const;
    // Queues every effectively visible item: solid mesh instances (or wire boxes for volumes without a
    // mesh), plus always-on-top outlines for selected items.
    // The renderer keeps the result, so this rebuilds only when the scene's RenderVersion() or the
    // axes setting changed (returns true then). Items that only moved (SceneTree::TransformChanges)
    // are patched in place instead.
    bool SubmitScene(const SceneTree& scene, ViewportRenderer& renderer);

    // True while a camera drag is in progress; the app keeps rendering continuously then.
    bool IsInteracting() const { return m_drag != Drag::None; }
    const char* InteractionName() const;

    const Rect& GetRect() const { return m_rect; }
    const EditorCamera& Camera() const { return m_camera; }
    EditorCamera& MutableCamera() { return m_camera; } // scripted control (benchmark)
    float FlySpeed() const { return m_flySpeed; }
    ViewportDrawSettings& DrawSettings() { return m_drawSettings; }
    const ViewportDrawSettings& DrawSettings() const { return m_drawSettings; }
    const TransformGizmo& Gizmo() const { return m_gizmo; }

    // Last click selection: time spent and items tested (stats overlay).
    double LastPickMs() const { return m_lastPickMs; }
    uint32_t LastPickItemsTested() const { return m_lastPickItems; }

private:
    enum class Drag { None, Orbit, Pan, Dolly, Fly };

    void BeginDrag(Drag drag, int mouseButton);
    void HandleShortcuts(SceneTree& scene);
    void DrawToolbar(float dpiScale);
    GizmoView MakeGizmoView(float dpiScale) const;
    NodeId PickAndSelect(SceneTree& scene, const Ray& ray, bool toggle, bool add);
    void FrameSelection(const SceneTree& scene); // F: fit the selection in view (origin if none)
    void PatchMovedItems(const SceneTree& scene, ViewportRenderer& renderer);

    EditorCamera m_camera;
    ViewportDrawSettings m_drawSettings;
    Rect m_rect;
    Drag m_drag = Drag::None;
    int m_dragButton = -1;
    bool m_hasKeyboardFocus = true; // viewport shortcuts active; starts focused
    bool m_toolbarHovered = false;
    float m_flySpeed = 8.0f; // world units per second

    const MeshLibrary* m_meshes = nullptr;
    TransformGizmo m_gizmo;
    GizmoView m_gizmoView;   // this frame's camera + mouse, for drawing the gizmo
    double m_lastPickMs = 0.0;
    uint32_t m_lastPickItems = 0;

    // What the renderer's retained scene layer was built from.
    uint64_t m_submittedRenderVersion = ~0ull;
    uint64_t m_submittedTransformVersion = ~0ull;
    size_t m_transformChangesApplied = 0; // entries of SceneTree::TransformChanges() already patched
    bool m_submittedShowAxes = false;
    // Where each submitted item lives in the renderer, indexed by NodeId.
    struct SubmittedItem {
        InstanceHandle mesh = kNoInstance;
        InstanceHandle outline = kNoInstance;
        uint32_t lines = kNoInstance; // first vertex of a volume's wire box in SceneLines()
    };
    std::vector<SubmittedItem> m_submitted;
};

} // namespace lm
