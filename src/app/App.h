#pragma once

#include "app/DebugOverlay.h"
#include "core/Profiler.h"
#include "editor/AssetPanel.h"
#include "editor/HierarchyPanel.h"
#include "editor/ModsPanel.h"
#include "editor/Viewport.h"
#include "mods/ModRuntime.h"
#include "platform/Window.h"
#include "render/GpuDevice.h"
#include "render/MeshLibrary.h"
#include "render/ViewportRenderer.h"
#include "scene/Commands.h"
#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

typedef unsigned int ImGuiID;

namespace lm {

struct AppOptions {
    uint32_t stressItems = 0;              // --stress N: add N items to the mock level
    uint32_t benchFrames = 0;              // --bench N: run the benchmark script (N frames per phase), then exit
    std::wstring benchOutput = L"bench.txt"; // --bench-out PATH
    std::vector<std::wstring> importFiles; // --import PATH (repeatable): import models at startup
    std::wstring mapFile;                   // --map PATH: load this map at startup
    std::wstring modsFolder;                // --mods PATH: mods folder (default: "mods" next to the exe)
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
    void DrawMainMenu();
    // Imports the files queued by drag & drop, File > Import Asset or --import.
    void ImportPendingFiles();
    bool ImportFile(const std::wstring& path, std::string& message); // true on success
    void ShowImportDialog(); // modal open-file dialog; queues the chosen files

    // ---- Map files (File menu). All of these may open modal dialogs: call them between frames.
    enum class FileAction { None, Load, Save, SaveAs };
    void RunFileAction(FileAction action);
    bool SaveMapAs();                                    // false if cancelled or failed
    bool SaveMapTo(const std::filesystem::path& path);   // false on failure (reported)
    bool LoadMapFrom(const std::filesystem::path& path); // false on failure (reported; scene unchanged)
    // Asks whether to save unsaved changes first. False if the user cancelled (or saving failed).
    bool ConfirmDiscardChanges(const wchar_t* action);
    bool HasUnsavedChanges() const;
    void MarkSaved();
    void UpdateWindowTitle();
    void SetFileStatus(std::string text, bool isError);
    void UndoStep();
    void RedoStep();
    void LoadMods(); // (re)loads every mod in m_modsFolder
    void DrawModsMenu();
    // Call after every modal OS dialog (file dialogs, message boxes): resets ImGui's key/mouse state.
    void AfterModalDialog();
    void MarkActive();
    DWORD IdleWaitTimeoutMs() const;

    bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    void OnWindowResized(uint32_t width, uint32_t height) override;
    void OnWindowDpiChanged(float dpiScale) override;
    void OnWindowLiveResize() override;
    void OnFilesDropped(std::span<const std::wstring> paths) override;
    bool OnWindowCloseRequested() override;

    Window m_window;
    GpuDevice m_gpu;
    MeshLibrary m_meshes;   // built-in + imported meshes; outlives the renderer that mirrors it
    AssetCatalog m_catalog; // what the Assets panel offers
    CommandHistory m_history; // undo / redo of every scene edit (last 100 steps)
    ViewportRenderer m_viewportRenderer;
    SceneTree m_scene;
    Viewport m_viewport;
    HierarchyPanel m_hierarchy;
    AssetPanel m_assets;
    std::vector<std::wstring> m_pendingImports;
    bool m_openImportDialog = false; // opened after the frame is presented (the dialog is modal)

    // Lua mods (mods/ folder): sandboxed, edit the scene through m_history like everything else.
    std::unique_ptr<ModRuntime> m_mods;
    std::filesystem::path m_modsFolder;
    ModsPanel m_modsPanel;
    bool m_reloadModsRequested = false;

    // Current map file (empty = untitled) and what was last saved or loaded, for "unsaved changes".
    std::filesystem::path m_mapPath;
    uint64_t m_savedContentVersion = 0;
    uint64_t m_savedImportVersion = 0;
    FileAction m_pendingFileAction = FileAction::None;
    std::string m_fileStatus; // shown at the right end of the menu bar
    bool m_fileStatusIsError = false;
    bool m_titleShowsUnsaved = false;
    std::filesystem::path m_titlePath;
    bool m_titleValid = false;
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
    NodeId m_benchMoveItem = kInvalidNode;
    std::array<BenchPhase, 3> m_benchPhases{};
};

} // namespace lm
