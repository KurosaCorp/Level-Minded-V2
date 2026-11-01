#include "app/App.h"

#include "core/AllocationCounter.h"
#include "core/Clock.h"
#include "import/ModelImporter.h"
#include "scene/MapFile.h"
#include "scene/MockLevel.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h> // DockBuilderGetCentralNode

#include <cderr.h>
#include <commdlg.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>

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

bool App::Init(const AppOptions& options)
{
    m_options = options;
    if (!m_window.Create({L"LevelMinded", 1600, 900}, this))
        return false;
    m_viewport.SetMeshLibrary(&m_meshes);
    m_viewport.SetCommandHistory(&m_history);
    if (!m_gpu.Create(m_window.Handle()) || !m_viewportRenderer.Create(m_gpu.Device(), &m_meshes))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(CountingImGuiAlloc, CountingImGuiFree, nullptr);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // no layout persistence yet
    ApplyStyle(m_window.DpiScale());

    if (!ImGui_ImplWin32_Init(m_window.Handle()) || !ImGui_ImplDX11_Init(m_gpu.Device(), m_gpu.Context()))
        return false;
    m_imguiReady = true;

    BuildMockLevel(m_scene);
    m_pendingImports = m_options.importFiles; // imported by the first frame
    if (m_options.stressItems > 0)
        BuildStressLevel(m_scene, m_options.stressItems);
    if (m_options.benchFrames > 0) {
        // The benchmark toggles selection of the largest top-level group every frame in phase 2.
        for (uint32_t i = 0; i < m_scene.Size(); i = m_scene.SubtreeEnd(i)) {
            if (m_benchToggleGroup == kInvalidNode ||
                m_scene.NodeAt(i).subtreeSize > m_scene.NodeAt(m_scene.IndexOf(m_benchToggleGroup)).subtreeSize)
                m_benchToggleGroup = m_scene.NodeAt(i).id;
        }
        // Phase 3 moves one item every frame, like dragging it with the gizmo.
        for (uint32_t i = m_scene.Size(); i-- > 0;) {
            if (!m_scene.NodeAt(i).IsGroup()) {
                m_benchMoveItem = m_scene.NodeAt(i).id;
                break;
            }
        }
        m_benchPhases[0].name = "orbit, static scene";
        m_benchPhases[1].name = "orbit + mass-select toggle every frame";
        m_benchPhases[2].name = "orbit + move one item every frame (gizmo drag)";
    }

    MarkSaved(); // the startup scene counts as unchanged
    m_window.Show();
    if (!m_options.mapFile.empty())
        LoadMapFrom(m_options.mapFile);
    UpdateWindowTitle();
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

        if (!m_window.PumpMessages() || m_deviceLost || m_quitRequested)
            break;
        if (m_options.benchFrames > 0)
            MarkActive(); // benchmark renders continuously

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
    const uint64_t heapAllocationsAtStart = HeapAllocationCount();
    const uint64_t imguiAllocationsAtStart = ImGuiAllocationCount();
    Profiler().BeginFrame();

    {
        ProfileScope zone(Zone::ImGuiNewFrame);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    if (m_options.benchFrames > 0)
        BenchBeforeFrame();

    // Menu bar first: the dockspace below fills what is left of the window.
    DrawMainMenu();
    // Models dropped on the window (or chosen in the import dialog) join the palette this frame.
    if (!m_pendingImports.empty())
        ImportPendingFiles();

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
    {
        ProfileScope zone(Zone::AssetPanel);
        const NodeId spawned = m_assets.Draw(m_scene, m_history, m_catalog, m_hierarchy.ActiveGroup(m_scene), dpiScale);
        if (spawned != kInvalidNode) {
            m_scene.ClearSelection();
            m_scene.SetSelected(spawned, true);
            m_hierarchy.SetActiveNode(spawned); // keeps the same target group for the next spawn
            m_hierarchy.Reveal(m_scene, spawned);
        }
    }
    {
        ProfileScope zone(Zone::HierarchyPanel);
        m_hierarchy.Draw(m_scene, m_history, dpiScale);
    }
    {
        ProfileScope zone(Zone::ViewportInput);
        const ViewportEvents events = m_viewport.Update(viewportRect, dpiScale, m_scene, m_hierarchy.ActiveNode());
        if (events.picked != kInvalidNode) { // clicked in the 3D view: make it the hierarchy's active node
            m_hierarchy.SetActiveNode(events.picked);
            m_hierarchy.Reveal(m_scene, events.picked);
        }
        m_viewport.DrawOverlays(ImGui::GetBackgroundDrawList(), dpiScale);
    }
    {
        ProfileScope zone(Zone::SceneSubmit);
        m_stats.sceneRebuilt = m_viewport.SubmitScene(m_scene, m_viewportRenderer);
    }
    {
        ProfileScope zone(Zone::Overlay);
        const OverlayInfo info{&m_gpu.AdapterName(), m_gpu.Width(), m_gpu.Height(), dpiScale, &m_scene};
        m_overlay.Draw(m_stats, info, m_viewport);
    }
    {
        ProfileScope zone(Zone::ImGuiBuild);
        ImGui::Render();
    }
    {
        ProfileScope zone(Zone::Render3D);
        m_gpu.BeginFrame(kClearColor);
        m_stats.viewportDrawCalls = m_viewportRenderer.Render(m_gpu.Context(), viewportRect, m_viewport.RenderCamera(),
                                                              m_viewport.DrawSettings());
        m_stats.stateChangesIssued = m_viewportRenderer.StateChangesIssued();
        m_stats.stateChangesSkipped = m_viewportRenderer.StateChangesSkipped();
        m_stats.bytesUploaded = m_viewportRenderer.BytesUploadedLastFrame();
    }
    {
        ProfileScope zone(Zone::RenderImGui);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    m_stats.cpuFrameMs = (Clock::Now() - frameStart) * 1000.0;
    Profiler().Add(Zone::Frame, m_stats.cpuFrameMs);
    Profiler().EndFrame();
    m_stats.heapAllocationsLastFrame = HeapAllocationCount() - heapAllocationsAtStart;
    m_stats.imguiAllocationsLastFrame = ImGuiAllocationCount() - imguiAllocationsAtStart;
    ++m_stats.framesRendered;
    if (m_options.benchFrames > 0)
        BenchAfterFrame(m_stats.heapAllocationsLastFrame, m_stats.imguiAllocationsLastFrame);

    if (m_gpu.Present() == PresentResult::DeviceLost)
        m_deviceLost = true;

    // Camera drags (e.g. fly mode with a key held) need frames even when no message arrives.
    if (m_viewport.IsInteracting())
        MarkActive();

    // The open-file dialog is modal (it runs its own message loop), so it opens between frames.
    if (m_openImportDialog) {
        m_openImportDialog = false;
        ShowImportDialog();
    }
    if (m_pendingFileAction != FileAction::None) {
        const FileAction action = m_pendingFileAction;
        m_pendingFileAction = FileAction::None;
        RunFileAction(action);
    }
    UpdateWindowTitle();
}

void App::DrawMainMenu()
{
    bool exitRequested = false;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load...", "Ctrl+O"))
                m_pendingFileAction = FileAction::Load;
            ImGui::SetItemTooltip("Open a LevelMinded map (.lmmap). The current map is replaced.");
            if (ImGui::MenuItem("Save", "Ctrl+S"))
                m_pendingFileAction = FileAction::Save;
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
                m_pendingFileAction = FileAction::SaveAs;
            ImGui::Separator();
            if (ImGui::MenuItem("Import Asset...", "Ctrl+I"))
                m_openImportDialog = true;
            ImGui::SetItemTooltip("Add .gltf, .glb or .obj models to the Assets palette.\n"
                                  "You can also drag model files onto the window.");
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                exitRequested = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            // Labels name the step ("Undo Move"); built only while the menu is open.
            char undoLabel[64], redoLabel[64];
            std::snprintf(undoLabel, sizeof(undoLabel), "Undo %s", m_history.CanUndo() ? m_history.UndoName() : "");
            std::snprintf(redoLabel, sizeof(redoLabel), "Redo %s", m_history.CanRedo() ? m_history.RedoName() : "");
            const bool busy = m_viewport.Gizmo().IsDragging();
            if (ImGui::MenuItem(undoLabel, "Ctrl+Z", false, m_history.CanUndo() && !busy))
                UndoStep();
            if (ImGui::MenuItem(redoLabel, "Ctrl+Y", false, m_history.CanRedo() && !busy))
                RedoStep();
            ImGui::Separator();
            ImGui::TextDisabled("History: %zu of %zu steps", m_history.UndoCount(), m_history.MaxSteps());
            ImGui::EndMenu();
        }
        // Result of the last save / load, right-aligned.
        if (!m_fileStatus.empty()) {
            const float width = ImGui::CalcTextSize(m_fileStatus.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
            ImGui::TextColored(m_fileStatusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s",
                               m_fileStatus.c_str());
        }
        ImGui::EndMainMenuBar();
    }

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I))
            m_openImportDialog = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
            m_pendingFileAction = FileAction::Load;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
            m_pendingFileAction = FileAction::Save;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
            m_pendingFileAction = FileAction::SaveAs;
        // Undo / redo (not while a gizmo drag is in progress: that drag isn't a step yet).
        if (!m_viewport.Gizmo().IsDragging()) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_Repeat))
                UndoStep();
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_Repeat) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_Repeat))
                RedoStep();
        }
    }
    if (exitRequested && ConfirmDiscardChanges(L"quitting"))
        m_quitRequested = true;
}

// ---------------------------------------------------------------------------------------- map files

namespace {

constexpr wchar_t kMapFilter[] = L"LevelMinded maps (*.lmmap)\0*.lmmap\0All files (*.*)\0*.*\0";

std::wstring DisplayName(const std::filesystem::path& mapPath)
{
    return mapPath.empty() ? std::wstring(L"Untitled") : mapPath.filename().wstring();
}

std::string Utf8Text(const std::wstring& text)
{
    const std::u8string utf8 = std::filesystem::path(text).u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string SizeText(size_t bytes)
{
    char text[32];
    if (bytes < 10 * 1024)
        std::snprintf(text, sizeof(text), "%zu bytes", bytes);
    else if (bytes < 10 * 1024 * 1024)
        std::snprintf(text, sizeof(text), "%.0f KB", double(bytes) / 1024.0);
    else
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return text;
}

} // namespace

void App::AfterModalDialog()
{
    // A modal OS dialog ran its own message loop between two frames. Key and button releases that
    // happened while it was open went to the dialog (e.g. the 'O' of the Ctrl+O that opened it), and
    // ImGui never sees a focus loss here (the focus-lost and focus-back events reach it in the same
    // frame, which cancel out). Without this, such keys would stay "held" and the next press of the
    // same shortcut would be ignored.
    if (m_imguiReady) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    MarkActive();
}

void App::UndoStep()
{
    const char* name = m_history.UndoName();
    if (name && m_history.Undo(m_scene))
        SetFileStatus(std::string("Undo: ") + name, false);
}

void App::RedoStep()
{
    const char* name = m_history.RedoName();
    if (name && m_history.Redo(m_scene))
        SetFileStatus(std::string("Redo: ") + name, false);
}

bool App::HasUnsavedChanges() const
{
    return m_scene.ContentVersion() != m_savedContentVersion || m_catalog.ImportVersion() != m_savedImportVersion;
}

void App::MarkSaved()
{
    m_savedContentVersion = m_scene.ContentVersion();
    m_savedImportVersion = m_catalog.ImportVersion();
}

void App::SetFileStatus(std::string text, bool isError)
{
    m_fileStatus = std::move(text);
    m_fileStatusIsError = isError;
}

void App::UpdateWindowTitle()
{
    // Rebuilt only when the map or its saved state changes (no per-frame string work).
    const bool unsaved = HasUnsavedChanges();
    if (m_titleValid && unsaved == m_titleShowsUnsaved && m_titlePath == m_mapPath)
        return;
    m_titleValid = true;
    m_titleShowsUnsaved = unsaved;
    m_titlePath = m_mapPath;
    const std::wstring title = DisplayName(m_mapPath) + (unsaved ? L"* - LevelMinded" : L" - LevelMinded");
    ::SetWindowTextW(m_window.Handle(), title.c_str());
}

bool App::ConfirmDiscardChanges(const wchar_t* action)
{
    if (!HasUnsavedChanges())
        return true;
    const std::wstring message = L"Save changes to " + DisplayName(m_mapPath) + L" before " + action + L"?";
    const int answer = ::MessageBoxW(m_window.Handle(), message.c_str(), L"LevelMinded", MB_YESNOCANCEL | MB_ICONWARNING);
    AfterModalDialog();
    if (answer == IDCANCEL)
        return false;
    if (answer == IDNO)
        return true;
    return m_mapPath.empty() ? SaveMapAs() : SaveMapTo(m_mapPath);
}

void App::RunFileAction(FileAction action)
{
    switch (action) {
    case FileAction::Save:
        if (m_mapPath.empty())
            SaveMapAs();
        else
            SaveMapTo(m_mapPath);
        break;
    case FileAction::SaveAs:
        SaveMapAs();
        break;
    case FileAction::Load: {
        if (!ConfirmDiscardChanges(L"loading another map"))
            break;
        std::vector<wchar_t> buffer(4096, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window.Handle();
        dialog.lpstrFilter = kMapFilter;
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrTitle = L"Load Map";
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        const BOOL chosen = ::GetOpenFileNameW(&dialog);
        AfterModalDialog();
        if (chosen)
            LoadMapFrom(buffer.data());
        break;
    }
    case FileAction::None:
        break;
    }
}

bool App::SaveMapAs()
{
    std::vector<wchar_t> buffer(4096, L'\0');
    const std::wstring initial = m_mapPath.empty() ? std::wstring(L"Untitled.lmmap") : m_mapPath.filename().wstring();
    std::copy(initial.begin(), initial.end(), buffer.begin());
    const std::wstring folder = m_mapPath.empty() ? std::wstring() : m_mapPath.parent_path().wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_window.Handle();
    dialog.lpstrFilter = kMapFilter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    dialog.lpstrTitle = L"Save Map As";
    dialog.lpstrDefExt = L"lmmap";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    const BOOL chosen = ::GetSaveFileNameW(&dialog);
    AfterModalDialog();
    return chosen && SaveMapTo(buffer.data());
}

bool App::SaveMapTo(const std::filesystem::path& path)
{
    const EditorCamera& camera = m_viewport.Camera();
    const MapCamera mapCamera{camera.Target(), camera.Yaw(), camera.Pitch(), camera.Distance()};
    const double start = Clock::Now();
    std::string error;
    MapSaveInfo info;
    if (!SaveMap(path, m_scene, m_catalog, mapCamera, error, &info)) {
        const std::wstring message = L"Could not save " + path.filename().wstring() + L":\n" +
                                     std::filesystem::path(std::u8string(error.begin(), error.end())).wstring();
        ::MessageBoxW(m_window.Handle(), message.c_str(), L"LevelMinded", MB_OK | MB_ICONERROR);
        AfterModalDialog();
        SetFileStatus("Save failed", true);
        return false;
    }
    m_mapPath = path;
    MarkSaved();
    char timing[64];
    std::snprintf(timing, sizeof(timing), " in %.1f ms", (Clock::Now() - start) * 1000.0);
    SetFileStatus("Saved " + Utf8Text(path.filename().wstring()) + ": " + WithThousandsSeparators(info.nodes) + " nodes, " +
                      SizeText(info.bytes) + timing,
                  false);
    MarkActive();
    return true;
}

bool App::LoadMapFrom(const std::filesystem::path& path)
{
    const double start = Clock::Now();
    std::string error;
    MapLoadReport report;
    if (!LoadMap(path, m_scene, m_catalog, m_meshes, report, error)) {
        const std::wstring message = L"Could not load " + path.filename().wstring() + L":\n" +
                                     std::filesystem::path(std::u8string(error.begin(), error.end())).wstring() +
                                     L"\n\nThe current map was not changed.";
        ::MessageBoxW(m_window.Handle(), message.c_str(), L"LevelMinded", MB_OK | MB_ICONERROR);
        AfterModalDialog();
        SetFileStatus("Load failed", true);
        return false;
    }
    const double ms = (Clock::Now() - start) * 1000.0;

    // Everything that held NodeIds or palette indices of the old map starts fresh.
    m_hierarchy.Reset();
    m_history.Clear(); // its steps refer to the previous map's nodes
    m_assets.Reset();
    m_pendingImports.clear();
    if (report.hasCamera) {
        EditorCamera& camera = m_viewport.MutableCamera();
        camera.SetOrientation(report.camera.yaw, report.camera.pitch);
        camera.SetTarget(report.camera.target);
        camera.SetDistance(report.camera.distance);
    }
    m_mapPath = path;
    MarkSaved();

    char timing[64];
    std::snprintf(timing, sizeof(timing), " in %.0f ms", ms);
    std::string status = "Loaded " + Utf8Text(path.filename().wstring()) + ": " + WithThousandsSeparators(report.nodes) +
                         " nodes, " + std::to_string(report.assets) + " models" + timing;
    if (!report.missingAssets.empty())
        status += " (" + std::to_string(report.missingAssets.size()) + " model files missing)";
    SetFileStatus(status, !report.missingAssets.empty());
    MarkActive();

    if (!report.missingAssets.empty()) {
        std::string names;
        for (size_t i = 0; i < report.missingAssets.size() && i < 10; ++i)
            names += "\n  - " + report.missingAssets[i];
        if (report.missingAssets.size() > 10)
            names += "\n  ...";
        const std::wstring message = L"These model files could not be found, so their items are shown as boxes:" +
                                     std::filesystem::path(std::u8string(names.begin(), names.end())).wstring() +
                                     L"\n\nMove the files back (next to the map, or to their old folder) and load the map again.";
        ::MessageBoxW(m_window.Handle(), message.c_str(), L"LevelMinded", MB_OK | MB_ICONWARNING);
        AfterModalDialog();
    }
    return true;
}

bool App::OnWindowCloseRequested()
{
    return ConfirmDiscardChanges(L"quitting");
}

void App::ShowImportDialog()
{
    // Multi-select result: "C:\dir\file.obj\0\0", or "C:\dir\0a.obj\0b.gltf\0\0".
    std::vector<wchar_t> buffer(32 * 1024, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_window.Handle();
    dialog.lpstrFilter = L"3D models (*.gltf, *.glb, *.obj)\0*.gltf;*.glb;*.obj\0glTF 2.0 (*.gltf, *.glb)\0*.gltf;*.glb\0"
                         L"Wavefront OBJ (*.obj)\0*.obj\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrTitle = L"Import Asset";
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    const BOOL chosen = ::GetOpenFileNameW(&dialog);
    AfterModalDialog();
    if (!chosen) {
        if (::CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
            m_assets.SetStatus("Too many files selected at once.", true);
        return;
    }

    const wchar_t* first = buffer.data();
    const wchar_t* next = first + std::wcslen(first) + 1;
    if (*next == L'\0') { // one file: full path
        m_pendingImports.emplace_back(first);
        return;
    }
    const std::filesystem::path folder(first);
    for (; *next != L'\0'; next += std::wcslen(next) + 1)
        m_pendingImports.push_back((folder / next).wstring());
}

void App::ImportPendingFiles()
{
    std::vector<std::wstring> files;
    files.swap(m_pendingImports);
    std::string message;
    std::string firstError;
    size_t imported = 0;
    const double start = Clock::Now();
    for (const std::wstring& file : files) {
        if (ImportFile(file, message))
            ++imported;
        else if (firstError.empty())
            firstError = message;
    }
    if (files.size() == 1) {
        m_assets.SetStatus(message, imported == 0);
    } else {
        char summary[96];
        std::snprintf(summary, sizeof(summary), "Imported %zu of %zu files in %.0f ms.", imported, files.size(),
                      (Clock::Now() - start) * 1000.0);
        m_assets.SetStatus(firstError.empty() ? std::string(summary) : std::string(summary) + " " + firstError, imported < files.size());
    }
}

namespace {

std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

bool App::ImportFile(const std::wstring& path, std::string& message)
{
    const std::filesystem::path file(path);
    const std::string fileName = Utf8(file.filename());
    std::error_code error;
    if (std::filesystem::is_directory(file, error)) {
        message = fileName + ": folders can't be imported; drop the model files themselves.";
        return false;
    }
    if (!IsImportableModel(file)) {
        message = fileName + ": unsupported file type (use .gltf, .glb or .obj).";
        return false;
    }
    if (const size_t existing = m_catalog.FindSource(file); existing != AssetCatalog::npos) {
        message = fileName + " is already in the palette.";
        m_assets.Highlight(existing);
        return true;
    }

    const double start = Clock::Now();
    ImportResult result = ImportModel(file);
    if (!result.ok) {
        message = "Could not import " + fileName + ": " + result.error;
        return false;
    }
    const ImportedModel& model = result.model;
    const MeshId mesh = m_meshes.Add(model.vertices, model.indices, model.traits);
    if (mesh == MeshId::None) {
        message = "Could not import " + fileName + ": the palette is full.";
        return false;
    }
    const double ms = (Clock::Now() - start) * 1000.0;

    const std::string triangles = WithThousandsSeparators(model.indices.size() / 3);
    const size_t index = m_catalog.AddImported(model.name, DescribeImportedModel(model, fileName), model.format, mesh,
                                               model.halfExtents, file);
    m_assets.Highlight(index);

    char timing[64];
    std::snprintf(timing, sizeof(timing), " in %.1f ms.", ms);
    message = "Imported " + std::string(m_catalog.Items()[index].name) + ": " + triangles + " triangles" + timing;
    if (model.skippedPrimitives > 0)
        message += " " + std::to_string(model.skippedPrimitives) + " part(s) were skipped (not triangles, or compressed).";
    return true;
}

namespace {
constexpr uint32_t kBenchWarmupFrames = 30;
}

void App::BenchBeforeFrame()
{
    // Scripted interaction: the camera orbits every frame; in phase 2 the largest group's selection
    // also flips every frame (100k+ items with --stress 100000); in phase 3 one item moves every frame.
    m_viewport.MutableCamera().Orbit(0.01f, 0.0f);
    if (m_benchFrame < kBenchWarmupFrames)
        return;
    const uint32_t phaseFrame = m_benchFrame - kBenchWarmupFrames;
    const uint32_t phase = phaseFrame / m_options.benchFrames;
    if (phase == 1 && m_benchToggleGroup != kInvalidNode) {
        const uint32_t index = m_scene.IndexOf(m_benchToggleGroup);
        m_scene.SetSelected(m_benchToggleGroup, !m_scene.NodeAt(index).Has(SceneTree::kSelected));
    } else if (phase == 2 && m_benchMoveItem != kInvalidNode) {
        Transform t = m_scene.TransformAt(m_scene.IndexOf(m_benchMoveItem));
        t.position.z += (phaseFrame & 1) ? 0.01f : -0.01f;
        m_scene.SetTransform(m_benchMoveItem, t);
    }
}

void App::BenchAfterFrame(uint64_t heapAllocations, uint64_t imguiAllocations)
{
    const uint32_t frame = m_benchFrame++;
    if (frame < kBenchWarmupFrames)
        return;
    const uint32_t phaseIndex = (frame - kBenchWarmupFrames) / m_options.benchFrames;
    if (phaseIndex >= m_benchPhases.size()) {
        WriteBenchReport();
        m_quitRequested = true;
        return;
    }
    BenchPhase& phase = m_benchPhases[phaseIndex];
    ++phase.frames;
    for (size_t zone = 0; zone < static_cast<size_t>(Zone::Count); ++zone) {
        const double ms = Profiler().LastMs(static_cast<Zone>(zone));
        phase.sumMs[zone] += ms;
        phase.maxMs[zone] = std::max(phase.maxMs[zone], ms);
    }
    phase.heapAllocations += heapAllocations;
    phase.imguiAllocations += imguiAllocations;
    phase.drawCalls = m_stats.viewportDrawCalls;
    phase.bytesUploaded += m_stats.bytesUploaded;
    phase.sceneRebuilds += m_stats.sceneRebuilt ? 1u : 0u;
    phase.stateChangesIssued = m_stats.stateChangesIssued;
    phase.stateChangesSkipped = m_stats.stateChangesSkipped;
}

void App::WriteBenchReport() const
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, m_options.benchOutput.c_str(), L"w") != 0 || !file)
        return;

    uint32_t items = 0;
    for (uint32_t i = 0; i < m_scene.Size(); ++i)
        items += m_scene.NodeAt(i).IsGroup() ? 0u : 1u;
    const SceneTree::MemoryUsage memory = m_scene.Memory();
    PROCESS_MEMORY_COUNTERS_EX processMemory{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&processMemory),
                           sizeof(processMemory));

    std::fprintf(file, "LevelMinded %s benchmark (%s)\n", LM_VERSION_STRING, LM_BUILD_CONFIG);
    std::fprintf(file, "Scene: %u nodes (%u items, %u groups)\n", m_scene.Size(), items, m_scene.Size() - items);
    std::fprintf(file, "Scene memory: %.2f MB total = nodes %.2f + names %.2f (%zu separate heap blocks) + items %.2f + index %.2f\n",
                 double(memory.Total()) / 1048576.0, double(memory.nodes) / 1048576.0, double(memory.names) / 1048576.0,
                 memory.nameAllocations, double(memory.items) / 1048576.0, double(memory.indexMap) / 1048576.0);
    std::fprintf(file, "Process: private %.1f MB, working set %.1f MB\n", double(processMemory.PrivateUsage) / 1048576.0,
                 double(processMemory.WorkingSetSize) / 1048576.0);
    std::fprintf(file, "Back buffer: %u x %u\n\n", m_gpu.Width(), m_gpu.Height());

    for (const BenchPhase& phase : m_benchPhases) {
        const double n = phase.frames ? double(phase.frames) : 1.0;
        std::fprintf(file, "Phase: %s (%u frames, %u draw calls)\n", phase.name, phase.frames, phase.drawCalls);
        std::fprintf(file, "  %-18s %9s %9s\n", "zone", "avg ms", "max ms");
        for (size_t zone = 0; zone < static_cast<size_t>(Zone::Count); ++zone)
            std::fprintf(file, "  %-18s %9.3f %9.3f\n", kZoneNames[zone], phase.sumMs[zone] / n, phase.maxMs[zone]);
        std::fprintf(file, "  heap allocations / frame  %.1f\n", double(phase.heapAllocations) / n);
        std::fprintf(file, "  ImGui allocations / frame %.1f\n", double(phase.imguiAllocations) / n);
        std::fprintf(file, "  GPU upload / frame        %.1f KB\n", double(phase.bytesUploaded) / n / 1024.0);
        std::fprintf(file, "  scene rebuilds            %u of %u frames\n", phase.sceneRebuilds, phase.frames);
        std::fprintf(file, "  3D state changes / frame  %u set, %u redundant skipped\n\n", phase.stateChangesIssued,
                     phase.stateChangesSkipped);
    }
    std::fclose(file);
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

void App::OnFilesDropped(std::span<const std::wstring> paths)
{
    m_pendingImports.insert(m_pendingImports.end(), paths.begin(), paths.end());
    MarkActive();
}

void App::OnWindowLiveResize()
{
    // The OS runs a modal loop while the border is dragged, so Run() is blocked; draw from here.
    if (m_imguiReady && !m_deviceLost)
        RenderFrame();
}

} // namespace lm
