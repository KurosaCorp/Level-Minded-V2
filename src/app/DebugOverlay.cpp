#include "app/DebugOverlay.h"

#include "core/Profiler.h"
#include "editor/Viewport.h"
#include "scene/SceneTree.h"

#include <imgui.h>

namespace lm {

void DebugOverlay::Draw(const FrameStats& stats, const OverlayInfo& info, Viewport& viewport)
{
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::GetIO().WantTextInput)
        m_visible = !m_visible;

    if (m_visible) {
        const Rect& rect = viewport.GetRect();
        const float pad = 10.0f * info.dpiScale;
        ImGui::SetNextWindowPos(ImVec2(rect.x + pad, rect.y + pad), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.65f);
        constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin("##DebugOverlay", nullptr, kFlags)) {
            ImGui::Text("LevelMinded %s (%s)", LM_VERSION_STRING, LM_BUILD_CONFIG);
            ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
            ImGui::Separator();
            ImGui::Text("GPU         %s", info.adapterName ? info.adapterName->c_str() : "?");
            ImGui::Text("Back buffer %u x %u", info.backBufferWidth, info.backBufferHeight);
            ImGui::Text("Viewport    %.0f x %.0f", rect.width, rect.height);
            ImGui::Text("DPI scale   %.0f%%", info.dpiScale * 100.0f);
            ImGui::Separator();
            ImGui::Text("CPU frame   %.3f ms", stats.cpuFrameMs);
            ImGui::Text("Draw calls  %u (3D)", stats.viewportDrawCalls);
            if (info.scene)
                ImGui::Text("Scene       %u nodes, %u selected", info.scene->Size(), info.scene->SelectedItemCount());
            ImGui::Text("Allocs      %llu heap, %llu ImGui (last frame)",
                        static_cast<unsigned long long>(stats.heapAllocationsLastFrame),
                        static_cast<unsigned long long>(stats.imguiAllocationsLastFrame));
            ImGui::Text("GPU upload  %.1f KB, state %u set / %u skipped", double(stats.bytesUploaded) / 1024.0,
                        stats.stateChangesIssued, stats.stateChangesSkipped);
            if (ImGui::TreeNodeEx("Profiler", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
                for (size_t zone = 0; zone < static_cast<size_t>(Zone::Count); ++zone)
                    ImGui::Text("  %-16s %7.3f ms", kZoneNames[zone], Profiler().LastMs(static_cast<Zone>(zone)));
            }
            ImGui::Text("Frames      %llu", static_cast<unsigned long long>(stats.framesRendered));
            ImGui::Text("Wakeups     %llu", static_cast<unsigned long long>(stats.wakeups));
            ImGui::Text("Last sleep  %.2f s", stats.lastSleepSeconds);
            ImGui::Separator();
            const EditorCamera& camera = viewport.Camera();
            const DirectX::XMFLOAT3 eye = camera.Eye();
            const DirectX::XMFLOAT3& target = camera.Target();
            ImGui::Text("Eye         %.2f, %.2f, %.2f", eye.x, eye.y, eye.z);
            ImGui::Text("Target      %.2f, %.2f, %.2f", target.x, target.y, target.z);
            ImGui::Text("Distance    %.2f", camera.Distance());
            ImGui::Text("Yaw/Pitch   %.1f / %.1f deg", DirectX::XMConvertToDegrees(camera.Yaw()),
                        DirectX::XMConvertToDegrees(camera.Pitch()));
            ImGui::Text("Fly speed   %.1f u/s", viewport.FlySpeed());
            ImGui::Text("Input       %s", viewport.InteractionName());
            static constexpr const char* kGizmoModes[] = {"Move", "Rotate", "Scale"};
            const TransformGizmo& gizmo = viewport.Gizmo();
            ImGui::Text("Gizmo       %s (%s), %u targets%s", kGizmoModes[static_cast<int>(gizmo.Mode())],
                        gizmo.Space() == GizmoSpace::Local ? "local" : "world", gizmo.TargetCount(),
                        gizmo.IsDragging() ? ", dragging" : "");
            ImGui::Text("Last pick   %.3f ms, %u items tested", viewport.LastPickMs(), viewport.LastPickItemsTested());
            {
                const ImGuiIO& io = ImGui::GetIO();
                const char* over = io.WantCaptureMouse                        ? "UI"
                                   : rect.Contains(io.MousePos.x, io.MousePos.y) ? "viewport"
                                                                                : "outside";
                ImGui::Text("Mouse       %.0f, %.0f (%s)", io.MousePos.x, io.MousePos.y, over);
            }
            ImGui::Separator();
            ViewportDrawSettings& settings = viewport.DrawSettings();
            ImGui::Checkbox("Grid", &settings.showGrid);
            ImGui::SameLine();
            ImGui::Checkbox("Axes", &settings.showAxes);
#ifndef IMGUI_DISABLE_DEMO_WINDOWS
            ImGui::SameLine();
            ImGui::Checkbox("ImGui demo", &m_showDemoWindow);
#endif
            if (ImGui::TreeNodeEx("Controls", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
                ImGui::TextDisabled("Alt+LMB orbit   MMB pan   Alt+RMB / wheel zoom");
                ImGui::TextDisabled("RMB + WASD/QE fly (Shift fast, wheel speed)");
                ImGui::TextDisabled("LMB select (Ctrl toggle, Shift add)   W/E/R move/rotate/scale   X world/local");
                ImGui::TextDisabled("Ctrl while dragging the gizmo snaps   Esc cancels the drag");
                ImGui::TextDisabled("F focus selection   Home reset");
                ImGui::TextDisabled("Numpad 1/3/7 front/right/top (Ctrl: opposite)");
                ImGui::TextDisabled("F1 toggle this overlay");
            }
        }
        ImGui::End();
    }

#ifndef IMGUI_DISABLE_DEMO_WINDOWS
    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);
#endif
}

} // namespace lm
