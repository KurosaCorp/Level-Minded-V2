#pragma once

#include "core/Clock.h"

#include <array>
#include <cstdint>
#include <iterator>

namespace lm {

// CPU zones of one frame, in the order they run.
enum class Zone : uint8_t {
    Frame,          // whole CPU frame (excludes waiting for the GPU / vsync)
    ImGuiNewFrame,  // backends + ImGui::NewFrame
    AssetPanel,
    HierarchyPanel,
    ViewportInput,  // camera input + orientation gizmo
    SceneSubmit,    // scene -> renderer instances/lines
    Overlay,
    ImGuiBuild,     // ImGui::Render (builds draw lists)
    Render3D,       // D3D11 commands for the viewport (incl. uploads)
    RenderImGui,    // D3D11 commands for the UI
    Count,
};

inline constexpr const char* kZoneNames[] = {
    "Frame (CPU)", "ImGui NewFrame", "Assets panel", "Hierarchy panel", "Viewport input",
    "Scene submit", "Overlay",       "ImGui build",  "3D render",       "ImGui render",
};
static_assert(std::size(kZoneNames) == static_cast<size_t>(Zone::Count));

// Per-frame CPU time per zone. Single-threaded, fixed size, no allocations.
class FrameProfiler {
public:
    void BeginFrame() { m_current.fill(0.0); }
    void EndFrame() { m_last = m_current; }
    void Add(Zone zone, double ms) { m_current[static_cast<size_t>(zone)] += ms; }
    double LastMs(Zone zone) const { return m_last[static_cast<size_t>(zone)]; }

private:
    std::array<double, static_cast<size_t>(Zone::Count)> m_current{};
    std::array<double, static_cast<size_t>(Zone::Count)> m_last{};
};

inline FrameProfiler& Profiler()
{
    static FrameProfiler profiler;
    return profiler;
}

class ProfileScope {
public:
    explicit ProfileScope(Zone zone) : m_zone(zone), m_start(Clock::Now()) {}
    ~ProfileScope() { Profiler().Add(m_zone, (Clock::Now() - m_start) * 1000.0); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    Zone m_zone;
    double m_start;
};

} // namespace lm
