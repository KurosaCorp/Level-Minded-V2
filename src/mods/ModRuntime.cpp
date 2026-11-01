#include "mods/ModRuntime.h"

// Lua is compiled as C++ (see cmake/Dependencies.cmake), so its headers are included without
// extern "C" and Lua errors are C++ exceptions.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

using namespace DirectX;

namespace lm {

namespace {

constexpr int kHookInterval = 10000; // instructions between budget checks
constexpr size_t kMaxLogLines = 200;
constexpr size_t kMaxMenuItemsPerMod = 64;

std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool ReadFile(const std::filesystem::path& path, std::string& out)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    _fseeki64(file, 0, SEEK_END);
    const long long length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 0 || length > (16 << 20)) { // a 16 MB script is not a mod
        std::fclose(file);
        return false;
    }
    out.resize(size_t(length));
    const bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

const char* KindName(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Group:  return "group";
    case NodeKind::Brush:  return "brush";
    case NodeKind::Mesh:   return "mesh";
    case NodeKind::Light:  return "light";
    case NodeKind::Entity: return "entity";
    }
    return "mesh";
}

} // namespace

struct ModRuntime::Mod {
    ModRuntime* runtime = nullptr;
    size_t index = 0;
    ModInfo info;
    lua_State* L = nullptr;
    uint64_t instructions = 0;                         // used by the current callback
    ModUi* ui = nullptr;                               // set during on_ui only
    std::vector<std::unique_ptr<Command>>* group = nullptr; // set during lm.undo_group
};

// ------------------------------------------------------------------------------------- the bindings

struct ModApi {
    static ModRuntime::Mod& Of(lua_State* L) { return **static_cast<ModRuntime::Mod**>(lua_getextraspace(L)); }
    static ModContext& Context(lua_State* L) { return Of(L).runtime->m_context; }
    static SceneTree& Scene(lua_State* L) { return *Context(L).scene; }

    // ---- Memory and time limits

    // The mod's heap: its own allocations, capped, never shared with the editor's allocators.
    static void* Alloc(void* user, void* pointer, size_t oldSize, size_t newSize)
    {
        auto* mod = static_cast<ModRuntime::Mod*>(user);
        const size_t old = pointer ? oldSize : 0;
        if (newSize == 0) {
            std::free(pointer);
            mod->info.memoryBytes -= old;
            return nullptr;
        }
        if (newSize > old && mod->info.memoryBytes - old + newSize > mod->runtime->m_limits.memoryBytes)
            return nullptr; // Lua turns this into a "not enough memory" error inside the mod
        void* resized = std::realloc(pointer, newSize);
        if (!resized)
            return nullptr;
        mod->info.memoryBytes = mod->info.memoryBytes - old + newSize;
        mod->info.peakMemoryBytes = std::max(mod->info.peakMemoryBytes, mod->info.memoryBytes);
        return resized;
    }

    static void CountHook(lua_State* L, lua_Debug*)
    {
        ModRuntime::Mod& mod = Of(L);
        mod.instructions += kHookInterval;
        if (mod.instructions > mod.runtime->m_limits.instructionsPerCall)
            luaL_error(L, "the mod ran for too long without returning (endless loop?) and was stopped");
    }

    // ---- Helpers

    static NodeId CheckNode(lua_State* L, int arg)
    {
        const lua_Integer value = luaL_checkinteger(L, arg);
        if (value < 0 || value > lua_Integer(UINT32_MAX) || !Scene(L).Contains(NodeId(value)))
            luaL_argerror(L, arg, "not a node in the scene (deleted, or from before a map was loaded?)");
        return NodeId(value);
    }

    static NodeId OptNode(lua_State* L, int arg) // nil = top level
    {
        return lua_isnoneornil(L, arg) ? kInvalidNode : CheckNode(L, arg);
    }

    static float CheckFloat(lua_State* L, int arg)
    {
        const lua_Number value = luaL_checknumber(L, arg);
        if (!std::isfinite(value))
            luaL_argerror(L, arg, "must be a finite number");
        return float(value);
    }

    // Reads {x, y, z} (or x/y/z fields) from the table at `index`.
    static XMFLOAT3 Vector3Field(lua_State* L, int index, const char* what)
    {
        float v[3];
        for (int i = 0; i < 3; ++i) {
            if (lua_rawgeti(L, index, i + 1) == LUA_TNIL) {
                lua_pop(L, 1);
                lua_getfield(L, index, i == 0 ? "x" : i == 1 ? "y" : "z");
            }
            if (!lua_isnumber(L, -1))
                luaL_error(L, "%s needs three numbers, like {1, 2, 3}", what);
            v[i] = float(lua_tonumber(L, -1));
            lua_pop(L, 1);
            if (!std::isfinite(v[i]))
                luaL_error(L, "%s must be finite", what);
        }
        return {v[0], v[1], v[2]};
    }

    static XMFLOAT4 EulerDegrees(float x, float y, float z) // about X, then Y, then Z (world axes)
    {
        const XMVECTOR qx = XMQuaternionRotationNormal(g_XMIdentityR0, XMConvertToRadians(x));
        const XMVECTOR qy = XMQuaternionRotationNormal(g_XMIdentityR1, XMConvertToRadians(y));
        const XMVECTOR qz = XMQuaternionRotationNormal(g_XMIdentityR2, XMConvertToRadians(z));
        XMFLOAT4 q;
        XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionMultiply(qx, qy), qz)));
        return q;
    }

    // Every scene edit: an undoable command, or part of the open lm.undo_group.
    static bool Run(lua_State* L, std::unique_ptr<Command> command)
    {
        ModRuntime::Mod& mod = Of(L);
        ModContext& context = Context(L);
        if (mod.group) {
            if (!command->Execute(*context.scene))
                return false;
            mod.group->push_back(std::move(command));
            return true;
        }
        return context.history->Execute(std::move(command), *context.scene) != nullptr;
    }

    static int SetTransform(lua_State* L, NodeId node, const Transform& transform, const char* name)
    {
        const Transform before = Scene(L).TransformAt(Scene(L).IndexOf(node));
        Run(L, std::make_unique<TransformCommand>(name, std::vector<NodeId>{node}, std::vector<Transform>{before},
                                                  std::vector<Transform>{transform}));
        return 0;
    }

    static NodeId CheckItem(lua_State* L, int arg)
    {
        const NodeId node = CheckNode(L, arg);
        if (Scene(L).NodeAt(Scene(L).IndexOf(node)).IsGroup())
            luaL_argerror(L, arg, "is a group (groups have no transform)");
        return node;
    }

    static void PushNodeList(lua_State* L, const std::vector<NodeId>& nodes)
    {
        lua_createtable(L, int(nodes.size()), 0);
        for (size_t i = 0; i < nodes.size(); ++i) {
            lua_pushinteger(L, lua_Integer(nodes[i]));
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
    }

    // ---- lm.*

    static int Log(lua_State* L)
    {
        std::string line;
        const int count = lua_gettop(L);
        for (int i = 1; i <= count; ++i) {
            size_t length = 0;
            const char* text = luaL_tolstring(L, i, &length);
            if (i > 1)
                line += '\t';
            line.append(text, length);
            lua_pop(L, 1);
        }
        Of(L).runtime->Log(Of(L), std::move(line));
        return 0;
    }

    static int Notify(lua_State* L)
    {
        const char* text = luaL_checkstring(L, 1);
        ModContext& context = Context(L);
        if (context.notify)
            context.notify(Of(L).info.name + ": " + text);
        return 0;
    }

    static int AddMenuItem(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        ModRuntime::Mod& mod = Of(L);
        ModRuntime& runtime = *mod.runtime;
        const auto mine = std::count_if(runtime.m_menu.begin(), runtime.m_menu.end(),
                                        [&](const ModRuntime::MenuItem& item) { return item.mod == mod.index; });
        if (size_t(mine) >= kMaxMenuItemsPerMod)
            return luaL_error(L, "too many menu items (at most %d per mod)", int(kMaxMenuItemsPerMod));
        lua_pushvalue(L, 2);
        const int function = luaL_ref(L, LUA_REGISTRYINDEX);
        runtime.m_menu.push_back({label, mod.index, function});
        return 0;
    }

    // lm.undo_group(name, fn): everything fn changes is one undo step; if fn fails, it is rolled back.
    static int UndoGroup(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        ModRuntime::Mod& mod = Of(L);
        if (mod.group) { // nested: part of the outer group
            lua_pushvalue(L, 2);
            lua_call(L, 0, 0);
            return 0;
        }
        std::vector<std::unique_ptr<Command>> commands;
        mod.group = &commands;
        lua_pushvalue(L, 2);
        const int status = lua_pcall(L, 0, 0, 0);
        mod.group = nullptr;
        ModContext& context = Context(L);
        if (status != LUA_OK) {
            for (auto it = commands.rbegin(); it != commands.rend(); ++it)
                (*it)->Undo(*context.scene);
            return lua_error(L); // re-raise with the original message
        }
        if (!commands.empty()) {
            auto macro = std::make_unique<MacroCommand>(name, std::move(commands));
            macro->MarkAllApplied();
            context.history->Push(std::move(macro));
        }
        return 0;
    }

    // ---- lm.ui.* (inside on_ui only)

    static ModUi& Ui(lua_State* L)
    {
        ModUi* ui = Of(L).ui;
        if (!ui)
            luaL_error(L, "lm.ui functions can only be used inside on_ui()");
        return *ui;
    }
    static int UiText(lua_State* L)
    {
        size_t length = 0;
        const char* text = luaL_tolstring(L, 1, &length);
        Ui(L).Text(text);
        return 0;
    }
    static int UiButton(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        lua_pushboolean(L, Ui(L).Button(label));
        return 1;
    }
    static int UiCheckbox(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        bool value = lua_toboolean(L, 2) != 0;
        Ui(L).Checkbox(label, value);
        lua_pushboolean(L, value);
        return 1;
    }
    static int UiSlider(lua_State* L)
    {
        const char* label = luaL_checkstring(L, 1);
        float value = CheckFloat(L, 2);
        const float min = CheckFloat(L, 3);
        const float max = CheckFloat(L, 4);
        if (!(min < max))
            return luaL_error(L, "lm.ui.slider: min must be less than max");
        Ui(L).Slider(label, value, min, max);
        lua_pushnumber(L, value);
        return 1;
    }
    static int UiSeparator(lua_State* L)
    {
        Ui(L).Separator();
        return 0;
    }
    static int UiSameLine(lua_State* L)
    {
        Ui(L).SameLine();
        return 0;
    }

    // ---- lm.scene.*

    // lm.scene.spawn(item, {name=, position={x,y,z}, rotation={x,y,z} degrees, scale=n or {x,y,z}, parent=})
    static int Spawn(lua_State* L)
    {
        const char* itemName = luaL_checkstring(L, 1);
        const AssetCatalog& catalog = *Context(L).catalog;
        const CatalogItem* item = nullptr;
        for (const CatalogItem& candidate : catalog.Items())
            if (std::strcmp(candidate.name, itemName) == 0)
                item = &candidate;
        if (!item)
            return luaL_argerror(L, 1, "no palette item with that name (see lm.assets.list())");

        std::string name = item->name;
        std::replace(name.begin(), name.end(), ' ', '_');
        Transform transform;
        NodeId parent = kInvalidNode;
        bool hasPosition = false;
        if (!lua_isnoneornil(L, 2)) {
            luaL_checktype(L, 2, LUA_TTABLE);
            if (lua_getfield(L, 2, "name") != LUA_TNIL)
                name = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "position") != LUA_TNIL) {
                luaL_checktype(L, -1, LUA_TTABLE);
                transform.position = Vector3Field(L, lua_gettop(L), "position");
                hasPosition = true;
            }
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "rotation") != LUA_TNIL) {
                luaL_checktype(L, -1, LUA_TTABLE);
                const XMFLOAT3 degrees = Vector3Field(L, lua_gettop(L), "rotation");
                transform.rotation = EulerDegrees(degrees.x, degrees.y, degrees.z);
            }
            lua_pop(L, 1);
            const int scaleType = lua_getfield(L, 2, "scale");
            if (scaleType == LUA_TNUMBER) {
                const float s = CheckFloat(L, -1);
                transform.scale = {s, s, s};
            } else if (scaleType == LUA_TTABLE) {
                transform.scale = Vector3Field(L, lua_gettop(L), "scale");
            } else if (scaleType != LUA_TNIL) {
                return luaL_error(L, "scale must be a number or {x, y, z}");
            }
            lua_pop(L, 1);
            for (float* s : {&transform.scale.x, &transform.scale.y, &transform.scale.z})
                if (std::abs(*s) < kMinScale)
                    return luaL_error(L, "scale must not be zero");
            if (lua_getfield(L, 2, "parent") != LUA_TNIL) {
                parent = CheckNode(L, lua_gettop(L));
                if (!Scene(L).NodeAt(Scene(L).IndexOf(parent)).IsGroup())
                    return luaL_error(L, "parent must be a group");
            }
            lua_pop(L, 1);
        }
        if (!hasPosition) // like the palette: standing on the ground at the origin
            transform.position = {0.0f, 0.0f, item->halfExtents.z * transform.scale.z};

        auto command = std::make_unique<CreateItemCommand>(name, item->kind, transform, item->halfExtents, parent, item->mesh);
        CreateItemCommand* raw = command.get();
        if (!Run(L, std::move(command)))
            return luaL_error(L, "the item could not be created");
        lua_pushinteger(L, lua_Integer(raw->Node()));
        return 1;
    }

    static int CreateGroup(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        const NodeId parent = OptNode(L, 2);
        if (parent != kInvalidNode && !Scene(L).NodeAt(Scene(L).IndexOf(parent)).IsGroup())
            return luaL_argerror(L, 2, "must be a group");
        auto command = std::make_unique<CreateGroupCommand>(name, parent);
        CreateGroupCommand* raw = command.get();
        if (!Run(L, std::move(command)))
            return luaL_error(L, "the group could not be created");
        lua_pushinteger(L, lua_Integer(raw->Node()));
        return 1;
    }

    static int Find(lua_State* L)
    {
        size_t length = 0;
        const char* name = luaL_checklstring(L, 1, &length);
        const SceneTree& scene = Scene(L);
        for (uint32_t i = 0; i < scene.Size(); ++i) {
            if (scene.NameAt(i) == std::string_view(name, length)) {
                lua_pushinteger(L, lua_Integer(scene.NodeAt(i).id));
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static int Exists(lua_State* L)
    {
        const lua_Integer value = luaL_checkinteger(L, 1);
        lua_pushboolean(L, value >= 0 && value <= lua_Integer(UINT32_MAX) && Scene(L).Contains(NodeId(value)));
        return 1;
    }

    static int Name(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        const std::string_view name = Scene(L).NameAt(Scene(L).IndexOf(node));
        lua_pushlstring(L, name.data(), name.size()); // copied into the mod's heap
        return 1;
    }

    static int Kind(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        lua_pushstring(L, KindName(Scene(L).NodeAt(Scene(L).IndexOf(node)).kind));
        return 1;
    }

    static int Parent(lua_State* L)
    {
        const SceneTree& scene = Scene(L);
        const uint32_t parent = scene.NodeAt(scene.IndexOf(CheckNode(L, 1))).parent;
        if (parent == SceneTree::kNoIndex)
            lua_pushnil(L);
        else
            lua_pushinteger(L, lua_Integer(scene.NodeAt(parent).id));
        return 1;
    }

    static int Children(lua_State* L)
    {
        const SceneTree& scene = Scene(L);
        const NodeId node = OptNode(L, 1);
        std::vector<NodeId> children;
        uint32_t first = 0, end = scene.Size();
        if (node != kInvalidNode) {
            first = scene.IndexOf(node) + 1;
            end = scene.SubtreeEnd(scene.IndexOf(node));
        }
        for (uint32_t i = first; i < end; i = scene.SubtreeEnd(i))
            children.push_back(scene.NodeAt(i).id);
        PushNodeList(L, children);
        return 1;
    }

    static int All(lua_State* L)
    {
        const SceneTree& scene = Scene(L);
        std::vector<NodeId> nodes;
        nodes.reserve(scene.Size());
        for (uint32_t i = 0; i < scene.Size(); ++i)
            nodes.push_back(scene.NodeAt(i).id);
        PushNodeList(L, nodes);
        return 1;
    }

    static int Position(lua_State* L)
    {
        const NodeId node = CheckItem(L, 1);
        const Transform& t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        lua_pushnumber(L, t.position.x);
        lua_pushnumber(L, t.position.y);
        lua_pushnumber(L, t.position.z);
        return 3;
    }

    static int SetPosition(lua_State* L)
    {
        const NodeId node = CheckItem(L, 1);
        Transform t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        t.position = {CheckFloat(L, 2), CheckFloat(L, 3), CheckFloat(L, 4)};
        return SetTransform(L, node, t, "Move");
    }

    static int Rotation(lua_State* L) // quaternion x, y, z, w
    {
        const NodeId node = CheckItem(L, 1);
        const Transform& t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        lua_pushnumber(L, t.rotation.x);
        lua_pushnumber(L, t.rotation.y);
        lua_pushnumber(L, t.rotation.z);
        lua_pushnumber(L, t.rotation.w);
        return 4;
    }

    static int SetRotation(lua_State* L) // degrees about X, then Y, then Z
    {
        const NodeId node = CheckItem(L, 1);
        Transform t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        t.rotation = EulerDegrees(CheckFloat(L, 2), CheckFloat(L, 3), CheckFloat(L, 4));
        return SetTransform(L, node, t, "Rotate");
    }

    static int Scale(lua_State* L)
    {
        const NodeId node = CheckItem(L, 1);
        const Transform& t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        lua_pushnumber(L, t.scale.x);
        lua_pushnumber(L, t.scale.y);
        lua_pushnumber(L, t.scale.z);
        return 3;
    }

    static int SetScale(lua_State* L) // one number = uniform
    {
        const NodeId node = CheckItem(L, 1);
        Transform t = Scene(L).TransformAt(Scene(L).IndexOf(node));
        const float x = CheckFloat(L, 2);
        const float y = lua_isnoneornil(L, 3) ? x : CheckFloat(L, 3);
        const float z = lua_isnoneornil(L, 4) ? x : CheckFloat(L, 4);
        for (float s : {x, y, z})
            if (std::abs(s) < kMinScale)
                return luaL_error(L, "scale must not be zero");
        t.scale = {x, y, z};
        return SetTransform(L, node, t, "Scale");
    }

    static int Visible(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        lua_pushboolean(L, Scene(L).NodeAt(Scene(L).IndexOf(node)).Has(SceneTree::kVisible));
        return 1;
    }

    static int SetVisible(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        Run(L, std::make_unique<VisibilityCommand>(node, lua_toboolean(L, 2) != 0));
        return 0;
    }

    static int Rename(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        size_t length = 0;
        const char* name = luaL_checklstring(L, 2, &length);
        if (length == 0 || length > 255)
            return luaL_argerror(L, 2, "names must be 1 to 255 bytes");
        Run(L, std::make_unique<RenameCommand>(node, std::string_view(name, length)));
        return 0;
    }

    static int Delete(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        Run(L, std::make_unique<DeleteCommand>(std::vector<NodeId>{node}));
        return 0;
    }

    static int Move(lua_State* L)
    {
        const NodeId node = CheckNode(L, 1);
        const NodeId parent = OptNode(L, 2);
        if (!Scene(L).CanMove(node, parent))
            return luaL_error(L, "can't move a node into itself, its own contents or an item");
        Run(L, std::make_unique<MoveCommand>(node, parent, kInvalidNode));
        return 0;
    }

    static int Selection(lua_State* L) // selected items (not groups), in hierarchy order
    {
        const SceneTree& scene = Scene(L);
        std::vector<NodeId> nodes;
        for (uint32_t i = 0; i < scene.Size(); ++i)
            if (!scene.NodeAt(i).IsGroup() && scene.NodeAt(i).Has(SceneTree::kSelected))
                nodes.push_back(scene.NodeAt(i).id);
        PushNodeList(L, nodes);
        return 1;
    }

    static int Select(lua_State* L) // handle, list of handles, or nil (clear); not an undo step
    {
        SceneTree& scene = Scene(L);
        std::vector<NodeId> nodes;
        if (lua_istable(L, 1)) {
            const lua_Integer count = luaL_len(L, 1);
            for (lua_Integer i = 1; i <= count; ++i) {
                lua_rawgeti(L, 1, i);
                nodes.push_back(CheckNode(L, -1));
                lua_pop(L, 1);
            }
        } else if (!lua_isnoneornil(L, 1)) {
            nodes.push_back(CheckNode(L, 1));
        }
        scene.ClearSelection();
        for (const NodeId node : nodes)
            scene.SetSelected(node, true);
        return 0;
    }

    static int AssetList(lua_State* L)
    {
        const std::span<const CatalogItem> items = Context(L).catalog->Items();
        lua_createtable(L, int(items.size()), 0);
        for (size_t i = 0; i < items.size(); ++i) {
            lua_pushstring(L, items[i].name);
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    }

    // ---- Building the sandbox

    static void SetFunctions(lua_State* L, const luaL_Reg* functions)
    {
        luaL_setfuncs(L, functions, 0);
    }

    static int OpenSandbox(lua_State* L)
    {
        // Safe standard libraries only. io, os, debug and package are not even compiled in.
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_settop(L, 0);
        // No loading of other files or of (unverifiable) binary chunks.
        for (const char* name : {"dofile", "loadfile", "load", "require"}) {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }
        lua_getglobal(L, LUA_STRLIBNAME);
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        lua_pop(L, 1);

        static const luaL_Reg kLm[] = {{"log", Log}, {"notify", Notify}, {"add_menu_item", AddMenuItem},
                                       {"undo_group", UndoGroup}, {nullptr, nullptr}};
        static const luaL_Reg kUi[] = {{"text", UiText},         {"button", UiButton},       {"checkbox", UiCheckbox},
                                       {"slider", UiSlider},     {"separator", UiSeparator}, {"same_line", UiSameLine},
                                       {nullptr, nullptr}};
        static const luaL_Reg kScene[] = {
            {"spawn", Spawn},         {"create_group", CreateGroup}, {"find", Find},         {"exists", Exists},
            {"name", Name},           {"kind", Kind},                {"parent", Parent},     {"children", Children},
            {"all", All},             {"position", Position},        {"set_position", SetPosition},
            {"rotation", Rotation},   {"set_rotation", SetRotation}, {"scale", Scale},       {"set_scale", SetScale},
            {"visible", Visible},     {"set_visible", SetVisible},   {"rename", Rename},     {"delete", Delete},
            {"move", Move},           {"selection", Selection},      {"select", Select},     {nullptr, nullptr}};
        static const luaL_Reg kAssets[] = {{"list", AssetList}, {nullptr, nullptr}};

        ModRuntime::Mod& mod = Of(L);
        lua_newtable(L);
        SetFunctions(L, kLm);
        lua_pushstring(L, mod.runtime->m_context.version.c_str());
        lua_setfield(L, -2, "version");
        lua_newtable(L);
        SetFunctions(L, kUi);
        lua_setfield(L, -2, "ui");
        lua_newtable(L);
        SetFunctions(L, kScene);
        lua_setfield(L, -2, "scene");
        lua_newtable(L);
        SetFunctions(L, kAssets);
        lua_setfield(L, -2, "assets");
        lua_setglobal(L, "lm");
        lua_pushcfunction(L, Log);
        lua_setglobal(L, "print"); // print goes to the mod's log
        return 0;
    }
};

// ----------------------------------------------------------------------------------------- runtime

ModRuntime::ModRuntime(ModContext context, ModLimits limits) : m_context(std::move(context)), m_limits(limits)
{
}

ModRuntime::~ModRuntime()
{
    UnloadAll();
}

const ModInfo& ModRuntime::Info(size_t mod) const
{
    return m_mods[mod]->info;
}

void ModRuntime::Log(Mod& mod, std::string line)
{
    mod.info.log.push_back(std::move(line));
    while (mod.info.log.size() > kMaxLogLines)
        mod.info.log.pop_front();
}

void ModRuntime::Fail(Mod& mod, std::string error)
{
    Log(mod, "error: " + error);
    mod.info.state = ModState::Failed;
    mod.info.error = std::move(error);
    mod.info.hasUi = false;
    std::erase_if(m_menu, [&](const MenuItem& item) { return item.mod == mod.index; });
    if (mod.L) { // free everything the mod allocated
        lua_close(mod.L);
        mod.L = nullptr;
    }
    mod.info.memoryBytes = 0;
}

bool ModRuntime::Call(Mod& mod, const char* function, int args)
{
    lua_State* L = mod.L;
    if (!L || mod.info.state != ModState::Running) {
        if (L)
            lua_pop(L, args);
        return false;
    }
    if (lua_getglobal(L, function) != LUA_TFUNCTION) {
        lua_pop(L, args + 1); // the mod doesn't define it: fine
        return true;
    }
    lua_insert(L, -(args + 1));
    mod.instructions = 0;
    if (lua_pcall(L, args, 0, 0) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        Fail(mod, std::string(function) + ": " + (message ? message : "error"));
        return false;
    }
    return true;
}

size_t ModRuntime::LoadFile(const std::filesystem::path& file)
{
    auto owned = std::make_unique<Mod>();
    Mod& mod = *owned;
    mod.runtime = this;
    mod.index = m_mods.size();
    mod.info.file = file;
    mod.info.name = Utf8(file.filename() == "main.lua" ? file.parent_path().filename() : file.stem());
    m_mods.push_back(std::move(owned));

    const auto seed = static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count());
    mod.L = lua_newstate(ModApi::Alloc, &mod, seed);
    if (!mod.L) {
        Fail(mod, "could not create a Lua state");
        return mod.index;
    }
    *static_cast<Mod**>(lua_getextraspace(mod.L)) = &mod;
    lua_sethook(mod.L, ModApi::CountHook, LUA_MASKCOUNT, kHookInterval);

    lua_pushcfunction(mod.L, ModApi::OpenSandbox);
    if (lua_pcall(mod.L, 0, 0, 0) != LUA_OK) {
        Fail(mod, "could not set up the sandbox");
        return mod.index;
    }

    std::string source;
    if (!ReadFile(file, source)) {
        Fail(mod, "the file could not be read");
        return mod.index;
    }
    if (source.size() >= 4 && std::memcmp(source.data(), LUA_SIGNATURE, 4) == 0) {
        Fail(mod, "precompiled Lua is not allowed; mods must be .lua source text");
        return mod.index;
    }
    const std::string chunkName = "=" + Utf8(file.filename());
    if (luaL_loadbufferx(mod.L, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK) {
        Fail(mod, lua_tostring(mod.L, -1) ? lua_tostring(mod.L, -1) : "syntax error");
        return mod.index;
    }
    mod.instructions = 0;
    if (lua_pcall(mod.L, 0, 0, 0) != LUA_OK) { // the file's top-level code
        Fail(mod, lua_tostring(mod.L, -1) ? lua_tostring(mod.L, -1) : "error");
        return mod.index;
    }

    // Optional metadata: mod_info = { name = "..." }.
    if (lua_getglobal(mod.L, "mod_info") == LUA_TTABLE && lua_getfield(mod.L, -1, "name") == LUA_TSTRING)
        mod.info.name = lua_tostring(mod.L, -1);
    lua_settop(mod.L, 0);
    lua_getglobal(mod.L, "on_ui");
    mod.info.hasUi = lua_isfunction(mod.L, -1);
    lua_pop(mod.L, 1);

    Call(mod, "on_load", 0);
    return mod.index;
}

size_t ModRuntime::LoadFolder(const std::filesystem::path& folder)
{
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error))
        return 0;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::filesystem::path& path = entry.path();
        std::wstring extension = path.extension().wstring();
        for (wchar_t& c : extension)
            c = wchar_t(std::towlower(c));
        if (entry.is_regular_file(error) && extension == L".lua")
            files.push_back(path);
        else if (entry.is_directory(error) && std::filesystem::is_regular_file(path / "main.lua", error))
            files.push_back(path / "main.lua");
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files)
        LoadFile(file);
    return files.size();
}

void ModRuntime::UnloadAll()
{
    for (const auto& mod : m_mods)
        Call(*mod, "on_unload", 0);
    for (const auto& mod : m_mods) {
        if (mod->L)
            lua_close(mod->L);
        mod->L = nullptr;
    }
    m_mods.clear();
    m_menu.clear();
}

void ModRuntime::Frame(double seconds)
{
    for (const auto& mod : m_mods) {
        if (!mod->L)
            continue;
        lua_pushnumber(mod->L, seconds);
        Call(*mod, "on_frame", 1);
    }
}

void ModRuntime::DrawUi(size_t index, ModUi& ui)
{
    if (index >= m_mods.size() || !m_mods[index]->info.hasUi)
        return;
    Mod& mod = *m_mods[index];
    mod.ui = &ui;
    Call(mod, "on_ui", 0);
    mod.ui = nullptr;
}

void ModRuntime::RunMenuItem(size_t index)
{
    if (index >= m_menu.size())
        return;
    const MenuItem item = m_menu[index]; // copy: a failing mod removes its items
    Mod& mod = *m_mods[item.mod];
    if (!mod.L || mod.info.state != ModState::Running)
        return;
    lua_rawgeti(mod.L, LUA_REGISTRYINDEX, item.function);
    mod.instructions = 0;
    if (lua_pcall(mod.L, 0, 0, 0) != LUA_OK) {
        const char* message = lua_tostring(mod.L, -1);
        Fail(mod, "menu item \"" + item.label + "\": " + (message ? message : "error"));
    }
}

} // namespace lm
