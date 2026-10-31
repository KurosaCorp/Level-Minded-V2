#pragma once

#include "core/Rect.h"
#include "editor/EditorCamera.h"
#include "render/ViewportRenderer.h"
#include "scene/SceneTree.h"

struct ImDrawList;

namespace lm {

// The central 3D view: owns the editor camera and turns mouse/keyboard input into camera moves.
//
// Controls (only when the cursor is over the viewport and not over UI):
//   Alt + LMB drag        orbit            MMB drag (or Alt + MMB)   pan
//   Alt + RMB drag        dolly            Mouse wheel               dolly
//   RMB drag              fly-look; WASD move, Q/E down/up, Shift fast, Ctrl slow, wheel = fly speed
//   F                     focus origin     Home                      reset view
//   Numpad 1 / 3 / 7      front / right / top view (Ctrl: back / left / bottom)
class Viewport {
public:
    // Processes input for this frame. Call between ImGui::NewFrame() and ImGui::Render().
    void Update(const Rect& rect, float dpiScale);
    // Axis orientation widget in the viewport's top-right corner.
    void DrawOrientationGizmo(ImDrawList* drawList, float dpiScale) const;

    ViewportCamera RenderCamera() const;
    // Queues every effectively visible item: solid mesh instances (or wire boxes for volumes without a
    // mesh), plus always-on-top outlines for selected items.
    // The renderer keeps the result, so this rebuilds only when the scene's RenderVersion() or the
    // axes setting changed. Returns true if it rebuilt.
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

private:
    enum class Drag { None, Orbit, Pan, Dolly, Fly };

    void BeginDrag(Drag drag, int mouseButton);
    void HandleShortcuts();

    EditorCamera m_camera;
    ViewportDrawSettings m_drawSettings;
    Rect m_rect;
    Drag m_drag = Drag::None;
    int m_dragButton = -1;
    bool m_hasKeyboardFocus = true; // viewport shortcuts active; starts focused
    float m_flySpeed = 8.0f; // world units per second

    // What the renderer's retained scene layer was built from.
    uint64_t m_submittedRenderVersion = ~0ull;
    bool m_submittedShowAxes = false;
};

} // namespace lm
