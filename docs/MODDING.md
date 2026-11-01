# Modding LevelMinded

LevelMinded runs **Lua mods**: small scripts that add buttons, menu commands and tools, and that can build or change the map through a safe API. Mods are plain text files. You don't need a compiler, the LevelMinded source code or any LevelMinded files to write one, which is exactly what section 3 of the [License](../README.md#license) allows you to share: mods that work with a user's own copy of LevelMinded.

Mods use **Lua 5.5** ([manual](https://www.lua.org/manual/5.5/)).

## Contents

- [Quick start](#quick-start)
- [Where mods live](#where-mods-live)
- [Callbacks](#callbacks)
- [API reference](#api-reference)
- [Undo](#undo)
- [Safety and limits](#safety-and-limits)
- [Errors and debugging](#errors-and-debugging)
- [Sharing mods](#sharing-mods)

## Quick start

1. Open the `mods` folder next to `LevelMinded.exe` (in LevelMinded: **Mods > Open Mods Folder**).
2. Create a file `my_first_mod.lua` with:

   ```lua
   mod_info = { name = "My First Mod" }

   function on_load()
       print("My first mod is running!")
   end

   function on_ui()
       if lm.ui.button("Place a cube") then
           lm.scene.spawn("Cube", { position = { 0, -14, 0.5 } })
       end
   end
   ```

3. Click **Reload** in the Mods panel (or **Mods > Reload Mods**), or restart LevelMinded.

Your mod appears in the **Mods** panel with its button. Clicking it places a cube, and **Ctrl+Z** removes it again.

The release package includes a complete example, `mods/hello_world.lua` (also in this repository under [`examples/mods/`](../examples/mods/hello_world.lua)).

## Where mods live

LevelMinded loads every mod in its `mods` folder when it starts, in alphabetical order:

| Layout | Mod name |
|---|---|
| `mods/my_tool.lua` | `my_tool` |
| `mods/my_tool/main.lua` (a folder with a `main.lua`) | `my_tool` |

Set `mod_info = { name = "..." }` in the file for a nicer display name. Other fields (author, version, ...) are allowed and ignored.

**Mods > Reload Mods** (or **Reload** in the Mods panel) unloads every mod and loads the folder again, so you can edit a mod and try it without restarting. A different folder can be used with the `--mods PATH` command-line option.

## Callbacks

Define any of these global functions. All are optional.

| Callback | When it runs |
|---|---|
| `on_load()` | Once, right after the mod is loaded (at startup, or after Reload). |
| `on_frame(seconds)` | Every frame LevelMinded draws, with the time since the previous frame. LevelMinded only draws while something is happening (it sleeps when idle), so don't rely on a steady rate. |
| `on_ui()` | When the mod's section of the Mods panel is drawn. Draw your controls here with `lm.ui`. |
| `on_unload()` | Before the mod is unloaded (Reload, or closing LevelMinded). |

The file's top-level code runs once when it is loaded, before `on_load`.

## API reference

Everything is in the global table `lm`. Positions are in **meters**, the world is **Z-up** (X red, Y green, Z blue), and angles are in **degrees**.

### General

| Function | Description |
|---|---|
| `lm.version` | LevelMinded's version, for example `"1.0.0"`. |
| `print(...)` / `lm.log(...)` | Writes a line to the mod's log (in the Mods panel). |
| `lm.notify(text)` | Shows a short message in the editor's status bar ("My Mod: text"). |
| `lm.add_menu_item(label, function)` | Adds a command to the **Mods** menu, under the mod's name. Up to 64 per mod. |
| `lm.undo_group(name, function)` | Runs `function`; every change it makes becomes **one** undo step called `name`. If the function raises an error, all its changes are rolled back. |

### `lm.ui` (only inside `on_ui`)

| Function | Returns |
|---|---|
| `lm.ui.text(text)` | Shows text (wrapped to the panel width). |
| `lm.ui.button(label)` | `true` on the frame the button is clicked. |
| `lm.ui.checkbox(label, value)` | The new value: `enabled = lm.ui.checkbox("Enabled", enabled)`. |
| `lm.ui.slider(label, value, min, max)` | The new value: `size = lm.ui.slider("Size", size, 1, 10)`. |
| `lm.ui.separator()` | A horizontal line. |
| `lm.ui.same_line()` | Puts the next control on the same line. |

Labels must be unique within your mod's panel. If two controls need the same visible text, add a hidden suffix: `"Delete##first"`, `"Delete##second"`.

### `lm.scene`

Nodes (groups and items) are referred to by **handles**: plain integers. A handle stays valid while the node exists, including when its deletion is undone, and becomes invalid when it is deleted or another map is loaded. Check with `lm.scene.exists(h)`. Passing an invalid handle raises an error.

**Changing the map.** Each call is one undo step (see [Undo](#undo)):

| Function | Description |
|---|---|
| `lm.scene.spawn(item, options)` | Places a palette item (built-in like `"Cube"`, or an imported model, by its palette name) and returns its handle. `options` is an optional table, described below. |
| `lm.scene.create_group(name, parent)` | Creates a group (at the top level if `parent` is `nil`) and returns its handle. |
| `lm.scene.set_position(h, x, y, z)` | Moves an item (its center). |
| `lm.scene.set_rotation(h, x, y, z)` | Sets an item's rotation: degrees about the world X, then Y, then Z axis. |
| `lm.scene.set_scale(h, x, y, z)` | Sets an item's scale (1 = its normal size). `set_scale(h, 2)` scales evenly. |
| `lm.scene.set_visible(h, visible)` | Shows or hides a node (hiding a group hides its contents). |
| `lm.scene.rename(h, name)` | Renames a node (1 to 255 bytes). |
| `lm.scene.move(h, parent)` | Moves a node (with its contents) into the group `parent`, or to the top level if `parent` is `nil`. |
| `lm.scene.delete(h)` | Deletes a node and everything in it. |

**`spawn` options:**

| Field | Default | Description |
|---|---|---|
| `name` | the item name, e.g. `"Floor_Tile"` | Name in the Hierarchy. |
| `position` | standing on the ground at the origin | `{x, y, z}`: where the item's center goes. |
| `rotation` | none | `{x, y, z}` in degrees (as in `set_rotation`). |
| `scale` | `1` | A number, or `{x, y, z}`. |
| `parent` | top level | Handle of the group to place it in. |

**Reading the map:**

| Function | Returns |
|---|---|
| `lm.scene.find(name)` | Handle of the first node with exactly this name, or `nil`. |
| `lm.scene.exists(h)` | Whether the node still exists. |
| `lm.scene.name(h)` | Its name. |
| `lm.scene.kind(h)` | `"group"`, `"brush"`, `"mesh"`, `"light"` or `"entity"`. |
| `lm.scene.parent(h)` | Handle of its group, or `nil` at the top level. |
| `lm.scene.children(h)` | List of the direct children of group `h` (top-level nodes if `h` is `nil`). |
| `lm.scene.all()` | List of every node, in Hierarchy order. |
| `lm.scene.position(h)` | `x, y, z` of an item. |
| `lm.scene.rotation(h)` | The item's rotation as a quaternion: `x, y, z, w`. |
| `lm.scene.scale(h)` | `x, y, z` scale of an item. |
| `lm.scene.visible(h)` | Whether the node's own eye toggle is on. |

**Selection** (not an undo step):

| Function | Description |
|---|---|
| `lm.scene.selection()` | List of the selected items. |
| `lm.scene.select(h)` | Selects `h` only. Also accepts a list of handles, or `nil` to clear the selection. |

### `lm.assets`

| Function | Returns |
|---|---|
| `lm.assets.list()` | Names of every palette item (built-in and imported), usable with `lm.scene.spawn`. |

### Standard Lua libraries

Available: the basic functions (`pairs`, `ipairs`, `tostring`, `pcall`, `error`, `assert`, `select`, `type`, `setmetatable`, ...), `string` (without `string.dump`), `table`, `math`, `utf8` and `coroutine`.

Not available: `io`, `os`, `debug`, `package` / `require`, `load`, `loadfile`, `dofile`. Mods can't read or write files, start programs, load native libraries or other code.

## Undo

Everything a mod changes in the map goes through LevelMinded's undo history, just like the user's own edits:

- each `lm.scene` call that changes something is one undo step;
- wrap related changes in `lm.undo_group` so the user can undo them with a single **Ctrl+Z**:

```lua
lm.undo_group("Build stairs", function()
    local stairs = lm.scene.create_group("Stairs")
    for step = 0, 9 do
        lm.scene.spawn("Cube", { parent = stairs, position = { 0, step, step * 0.5 + 0.25 },
                                 scale = { 1, 0.5, 0.25 } })
    end
end)
```

If anything inside the group fails, the changes it already made are rolled back and the error is passed on, so a half-built result is never left behind.

## Safety and limits

Mods run in a sandbox so that a broken or malicious mod can't harm the editor, the map or the computer:

- **Own memory.** Each mod runs in its own Lua state with its own heap, separate from the editor's memory. Mods never receive pointers into editor memory: they get integer handles and copies of names and numbers. Every handle and argument is checked before it is used.
- **Memory limit.** A mod may use up to **64 MB**. Going over stops the mod with an "out of memory" error.
- **Time limit.** A single callback may run for about **200 million Lua instructions** (well under a second). A mod stuck in an endless loop is stopped instead of freezing the editor.
- **Source text only.** Precompiled Lua bytecode is refused (it can't be verified).
- **No system access.** No files, network, processes or native code (see [Standard Lua libraries](#standard-lua-libraries)).
- **Isolation.** When a mod fails, only that mod stops. Its error is shown, the memory it used is freed, and every other mod keeps running.

## Errors and debugging

- `print` / `lm.log` output and errors appear in the mod's **Log** in the Mods panel (the last 200 lines).
- When a mod raises an error that it doesn't catch with `pcall`, it is stopped. The Mods panel shows the message with the file and line, for example `hello_world.lua:42: attempt to index a nil value`. Fix the file and click **Reload**.
- The status bar reports at startup when mods failed to load.

## Sharing mods

Share your `.lua` file, or your mod's folder. Per section 3 of the [License](../README.md#license), a mod must not contain any original LevelMinded code, executables or assets. It relies on the user's own copy of LevelMinded (as every Lua mod does), and must not be distributed as a standalone version of LevelMinded.
