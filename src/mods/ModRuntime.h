#pragma once

#include "scene/Commands.h"
#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace lm {

// What mods draw their UI with (ImGui in the editor, a recorder in the tests). Labels are
// null-terminated UTF-8 owned by the mod's Lua state, valid for the duration of the call.
class ModUi {
public:
    virtual ~ModUi() = default;
    virtual void Text(const char* text) = 0;
    virtual bool Button(const char* label) = 0;
    virtual bool Checkbox(const char* label, bool& value) = 0;
    virtual bool Slider(const char* label, float& value, float min, float max) = 0;
    virtual void Separator() = 0;
    virtual void SameLine() = 0;
};

// The parts of the editor mods may use, always through the API's checked wrappers: mods never get a
// pointer into editor memory, only integer node handles and copied values, and every edit they make
// is a regular undoable Command.
struct ModContext {
    SceneTree* scene = nullptr;
    CommandHistory* history = nullptr;
    const AssetCatalog* catalog = nullptr;
    std::function<void(const std::string&)> notify; // lm.notify(): a short message in the status bar
    std::string version;                            // lm.version
};

// Per-mod resource limits. A mod that exceeds one fails (with an error message) instead of
// slowing down or crashing the editor; other mods keep running.
struct ModLimits {
    size_t memoryBytes = size_t(64) << 20;      // the mod's own Lua heap (separate from the editor's)
    uint64_t instructionsPerCall = 200'000'000; // per callback; stops endless loops (~0.5-1 s)
};

enum class ModState : uint8_t { Running, Failed };

struct ModInfo {
    std::string name; // mod_info.name, or the file / folder name
    std::filesystem::path file;
    ModState state = ModState::Running;
    std::string error;            // why it failed
    std::deque<std::string> log;  // lm.log / print output and errors, newest last (bounded)
    size_t memoryBytes = 0;       // current Lua heap
    size_t peakMemoryBytes = 0;
    bool hasUi = false;           // defines on_ui()
};

// Lua mods (see docs/MODDING.md). Each mod is a .lua file, or a folder with a main.lua, and runs in
// its own Lua state: a sandbox with only the safe standard libraries (no io, os, debug, package,
// load/dofile; text chunks only), a capped heap and an instruction budget per call. A mod that
// raises an error is stopped and reported; it can't take the editor or other mods down with it.
//
// Callbacks a mod may define: on_load(), on_frame(seconds), on_ui(), on_unload().
class ModRuntime {
public:
    explicit ModRuntime(ModContext context, ModLimits limits = {});
    ~ModRuntime();
    ModRuntime(const ModRuntime&) = delete;
    ModRuntime& operator=(const ModRuntime&) = delete;

    // Loads every mod in `folder` (*.lua files and sub-folders with a main.lua, in name order) and
    // runs their on_load. A missing folder is fine. Returns the number of mods found.
    size_t LoadFolder(const std::filesystem::path& folder);
    // Loads one mod and runs its on_load. Returns its index; a mod that fails stays listed with its error.
    size_t LoadFile(const std::filesystem::path& file);
    // Calls on_unload and closes every mod (frees all mod memory).
    void UnloadAll();

    void Frame(double seconds);            // on_frame(seconds) of every running mod
    void DrawUi(size_t mod, ModUi& ui);    // that mod's on_ui(), drawing with `ui`

    struct MenuItem {
        std::string label;
        size_t mod;
        int function; // registry reference in the mod's state
    };
    const std::vector<MenuItem>& MenuItems() const { return m_menu; } // from lm.add_menu_item
    void RunMenuItem(size_t index);

    size_t Count() const { return m_mods.size(); }
    const ModInfo& Info(size_t mod) const;
    const ModLimits& Limits() const { return m_limits; }

    struct Mod; // internal; public so the Lua bindings in ModRuntime.cpp can name it

private:
    friend struct ModApi;
    bool Call(Mod& mod, const char* function, int args); // global function; false if it failed
    void Fail(Mod& mod, std::string error);
    void Log(Mod& mod, std::string line);

    ModContext m_context;
    ModLimits m_limits;
    std::vector<std::unique_ptr<Mod>> m_mods;
    std::vector<MenuItem> m_menu;
};

} // namespace lm
