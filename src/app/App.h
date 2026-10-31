#pragma once

#include "app/DebugOverlay.h"
#include "core/Profiler.h"
#include "editor/AssetPanel.h"
#include "editor/HierarchyPanel.h"
#include "editor/Viewport.h"
#include "platform/Window.h"
#include "render/GpuDevice.h"
#include "render/ViewportRenderer.h"
#include "scene/SceneTree.h"

#include <array>
#include <string>

typedef unsigned int ImGuiID;

namespace lm {

struct AppOptions {
    uint32_t stressItems = 0;              // --stress N: add N items to the mock level
    uint32_t benchFrames = 0;              // --bench N: run the benchmark script (N frames per phase), then exit
    std::wstring benchOutput = L"bench.txt"; // --bench-out PATH
};

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

    bool Init(const AppOptions& options = {});
    int Run();

private:
    // Accumulated per-zone CPU times for one benchmark phase.
    struct BenchPhase {
        const char* name = "";
        uint32_t frames = 0;
        std::array<double, static_cast<size_t>(Zone::Count)> sumMs{};
        std::array<double, static_cast<size_t>(Zone::Count)> maxMs{};
        uint64_t heapAllocations = 0;
        uint64_t imguiAllocations = 0;
        uint64_t bytesUploaded = 0;
        uint32_t drawCalls = 0;
        uint32_t sceneRebuilds = 0;
        uint32_t stateChangesIssued = 0;
        uint32_t stateChangesSkipped = 0;
    };

    void Shutdown();
    void RenderFrame();
    void BenchBeforeFrame();
    void BenchAfterFrame(uint64_t heapAllocations, uint64_t imguiAllocations);
    void WriteBenchReport() const;
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

    AppOptions m_options;
    bool m_quitRequested = false;
    uint32_t m_benchFrame = 0;
    NodeId m_benchToggleGroup = kInvalidNode;
    std::array<BenchPhase, 2> m_benchPhases{};
};

} // namespace lm
