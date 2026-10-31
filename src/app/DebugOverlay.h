#pragma once

#include <cstdint>
#include <string>

namespace lm {

class Viewport;
class SceneTree;

struct FrameStats {
    uint64_t framesRendered = 0;
    uint64_t wakeups = 0;            // times the main loop woke up from an idle wait
    double lastSleepSeconds = 0;     // length of the most recent idle wait
    double cpuFrameMs = 0;           // CPU time to build and submit the last frame
    uint32_t viewportDrawCalls = 0;  // 3D draw calls in the last frame (ImGui excluded)
};

struct OverlayInfo {
    const std::string* adapterName = nullptr;
    uint32_t backBufferWidth = 0;
    uint32_t backBufferHeight = 0;
    float dpiScale = 1.0f;
    const SceneTree* scene = nullptr;
};

// Small panel in the viewport's top-left corner with build, GPU, frame-loop and camera stats.
// F1 toggles it.
class DebugOverlay {
public:
    void Draw(const FrameStats& stats, const OverlayInfo& info, Viewport& viewport);

private:
    bool m_visible = true;
    bool m_showDemoWindow = false;
};

} // namespace lm
