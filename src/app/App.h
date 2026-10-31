#pragma once

#include "app/DebugOverlay.h"
#include "editor/AssetPanel.h"
#include "editor/HierarchyPanel.h"
#include "editor/Viewport.h"
#include "platform/Window.h"
#include "render/GpuDevice.h"
#include "render/ViewportRenderer.h"
#include "scene/SceneTree.h"

typedef unsigned int ImGuiID;

namespace lm {

// Owns the window, GPU device and UI, and runs the event-driven main loop.
//
// The loop only renders while something is happening. Each window message (input, resize,
// focus, ...) keeps the app "active" for a short settle period so UI animations and
// hover delays can finish; after that the thread blocks in the OS until the next message,
// so an idle editor costs no CPU or GPU time.
class App final : public WindowEventHandler {
public:
    App() = default;
    ~App() override;

    bool Init();
    int Run();

private:
    void Shutdown();
    void RenderFrame();
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void ApplyStyle(float dpiScale);
    void MarkActive();
    DWORD IdleWaitTimeoutMs() const;

    bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    void OnWindowResized(uint32_t width, uint32_t height) override;
    void OnWindowDpiChanged(float dpiScale) override;
    void OnWindowLiveResize() override;

    Window m_window;
    GpuDevice m_gpu;
    ViewportRenderer m_viewportRenderer;
    SceneTree m_scene;
    Viewport m_viewport;
    HierarchyPanel m_hierarchy;
    AssetPanel m_assets;
    DebugOverlay m_overlay;
    FrameStats m_stats;

    bool m_imguiReady = false;
    bool m_layoutBuilt = false;
    bool m_deviceLost = false;
    uint32_t m_pendingWidth = 0;
    uint32_t m_pendingHeight = 0;
    double m_activeUntil = 0.0; // keep rendering until this time (Clock::Now seconds)
};

} // namespace lm
