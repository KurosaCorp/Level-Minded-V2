#include "app/App.h"

#include "core/Clock.h"
#include "scene/MockLevel.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h> // DockBuilderGetCentralNode

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lm {

namespace {

// How long to keep rendering after the last window message. Covers ImGui hover delays
// (tooltips appear after ~0.4 s) and window fade/auto-size settling.
constexpr double kSettleSeconds = 0.5;
// Wake-up interval while a text field is focused, so the caret keeps blinking.
constexpr DWORD kCaretBlinkWaitMs = 250;
// Poll interval while the swap chain is occluded (e.g. screen locked); unlocking sends no message.
constexpr DWORD kOccludedWaitMs = 250;

constexpr float kClearColor[4] = {0.09f, 0.09f, 0.10f, 1.0f};

// Queries that change no state. Some are sent to us by our own frame (the ImGui Win32 backend
// calls WindowFromPoint each frame, which sends WM_NCHITTEST), so treating them as activity
// would keep the loop rendering forever.
bool IsActivityMessage(UINT msg)
{
    switch (msg) {
    case WM_NCHITTEST:
    case WM_GETOBJECT:
    case WM_GETICON:
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_GETMINMAXINFO:
        return false;
    default:
        return true;
    }
}

} // namespace

App::~App()
{
    Shutdown();
}

bool App::Init()
{
    if (!m_window.Create({L"LevelMinded", 1600, 900}, this))
        return false;
    if (!m_gpu.Create(m_window.Handle()) || !m_viewportRenderer.Create(m_gpu.Device()))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // no layout persistence yet
    ApplyStyle(m_window.DpiScale());

    if (!ImGui_ImplWin32_Init(m_window.Handle()) || !ImGui_ImplDX11_Init(m_gpu.Device(), m_gpu.Context()))
        return false;
    m_imguiReady = true;

    BuildMockLevel(m_scene);

    m_window.Show();
    MarkActive();
    return true;
}

void App::Shutdown()
{
    if (m_imguiReady) {
        m_imguiReady = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    m_gpu.Destroy();
    m_window.Destroy();
}

int App::Run()
{
    for (;;) {
        const bool minimized = m_window.IsMinimized();
        const bool occluded = !minimized && m_gpu.IsOccluded();
        const bool active = !minimized && !occluded && Clock::Now() < m_activeUntil;

        if (!active) {
            // Idle: block in the OS until a message arrives (or a timeout for caret blink / occlusion).
            const DWORD timeout = minimized ? INFINITE : occluded ? kOccludedWaitMs : IdleWaitTimeoutMs();
            const double sleepStart = Clock::Now();
            m_window.WaitForEvents(timeout);
            m_stats.lastSleepSeconds = Clock::Now() - sleepStart;
            ++m_stats.wakeups;
        }

        if (!m_window.PumpMessages() || m_deviceLost)
            break;

        // Render at least one frame per wakeup (a timeout wakeup carries no message).
        if (!m_window.IsMinimized() && !m_gpu.IsOccluded())
            RenderFrame();
    }

    if (m_deviceLost) {
        ::MessageBoxW(nullptr, L"The GPU device was lost (driver reset or GPU removed).", L"LevelMinded",
                      MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}

void App::RenderFrame()
{
    if (m_pendingWidth != 0 && m_pendingHeight != 0) {
        m_gpu.Resize(m_pendingWidth, m_pendingHeight);
        m_pendingWidth = m_pendingHeight = 0;
    }

    m_gpu.WaitForNextFrame();
    const double frameStart = Clock::Now();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Dockspace over the whole window. Its central node stays transparent and passes input through;
    // the 3D viewport is drawn there, directly into the back buffer (no extra render target or copy).
    const float dpiScale = m_window.DpiScale();
    const ImGuiID dockspaceId = ImGui::GetID("LevelMindedDockSpace");
    if (!m_layoutBuilt) {
        BuildDefaultLayout(dockspaceId);
        m_layoutBuilt = true;
    }
    ImGui::DockSpaceOverViewport(dockspaceId, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    Rect viewportRect{0.0f, 0.0f, static_cast<float>(m_gpu.Width()), static_cast<float>(m_gpu.Height())};
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
        viewportRect = {central->Pos.x, central->Pos.y, central->Size.x, central->Size.y};

    // Assets first: an item spawned here shows up in the Hierarchy and the viewport this same frame.
    const NodeId spawned = m_assets.Draw(m_scene, m_hierarchy.ActiveGroup(m_scene), dpiScale);
    if (spawned != kInvalidNode) {
        m_scene.ClearSelection();
        m_scene.SetSelected(spawned, true);
        m_hierarchy.SetActiveNode(spawned); // keeps the same target group for the next spawn
        m_hierarchy.Reveal(m_scene, spawned);
    }
    m_hierarchy.Draw(m_scene, dpiScale);
    m_viewport.Update(viewportRect, dpiScale);
    m_viewport.DrawOrientationGizmo(ImGui::GetBackgroundDrawList(), dpiScale);
    m_viewport.SubmitScene(m_scene, m_viewportRenderer);

    const OverlayInfo info{&m_gpu.AdapterName(), m_gpu.Width(), m_gpu.Height(), dpiScale, &m_scene};
    m_overlay.Draw(m_stats, info, m_viewport);

    ImGui::Render();
    m_gpu.BeginFrame(kClearColor);
    m_stats.viewportDrawCalls = m_viewportRenderer.Render(m_gpu.Context(), viewportRect, m_viewport.RenderCamera(),
                                                          m_viewport.DrawSettings());
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_stats.cpuFrameMs = (Clock::Now() - frameStart) * 1000.0;
    ++m_stats.framesRendered;

    if (m_gpu.Present() == PresentResult::DeviceLost)
        m_deviceLost = true;

    // Camera drags (e.g. fly mode with a key held) need frames even when no message arrives.
    if (m_viewport.IsInteracting())
        MarkActive();
}

void App::BuildDefaultLayout(ImGuiID dockspaceId)
{
    // Hierarchy on the left, Assets on the right; the remaining central node is the 3D viewport.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID center = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.2f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, &right, &center);
    ImGui::DockBuilderDockWindow(HierarchyPanel::kWindowName, left);
    ImGui::DockBuilderDockWindow(AssetPanel::kWindowName, right);
    ImGui::DockBuilderFinish(dockspaceId);
}

void App::ApplyStyle(float dpiScale)
{
    // Rebuild from defaults so repeated DPI changes don't compound the scaling.
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;
    ImGui::GetStyle() = style;
}

void App::MarkActive()
{
    m_activeUntil = Clock::Now() + kSettleSeconds;
}

DWORD App::IdleWaitTimeoutMs() const
{
    if (m_imguiReady && ImGui::GetIO().WantTextInput)
        return kCaretBlinkWaitMs;
    return INFINITE;
}

bool App::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (IsActivityMessage(msg))
        MarkActive();
    return m_imguiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
}

void App::OnWindowResized(uint32_t width, uint32_t height)
{
    m_pendingWidth = width;
    m_pendingHeight = height;
}

void App::OnWindowDpiChanged(float dpiScale)
{
    if (m_imguiReady)
        ApplyStyle(dpiScale);
}

void App::OnWindowLiveResize()
{
    // The OS runs a modal loop while the border is dragged, so Run() is blocked; draw from here.
    if (m_imguiReady && !m_deviceLost)
        RenderFrame();
}

} // namespace lm
