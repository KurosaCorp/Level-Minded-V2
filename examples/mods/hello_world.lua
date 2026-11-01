-- Hello World: a sample LevelMinded mod.
--
-- Mods are Lua files in the "mods" folder next to LevelMinded.exe (or folders there with a main.lua).
-- They load automatically when LevelMinded starts; Mods > Reload Mods loads them again after edits.
-- Everything a mod does to the map goes through the editor's undo history (Ctrl+Z undoes it).
-- API reference: docs/MODDING.md

mod_info = { name = "Hello World", author = "KurosaCorp", version = "1.0" }

local spawned = 0
local spacing = 2.0

-- Mods > Hello World > Spawn a ring of cubes: eight cubes in a new group, as one undo step.
local function spawn_ring()
    lm.undo_group("Hello World ring", function()
        local group = lm.scene.create_group("Hello Ring")
        for i = 1, 8 do
            local angle = (i - 1) / 8 * 2 * math.pi
            lm.scene.spawn("Cube", {
                name = "Ring_Cube_" .. i,
                parent = group,
                position = { 6 * math.cos(angle), 6 * math.sin(angle), 0.5 },
                rotation = { 0, 0, math.deg(angle) },
            })
        end
    end)
    lm.notify("spawned a ring of 8 cubes")
end

-- Runs once, right after the mod is loaded.
function on_load()
    print("Hello, World! Running in LevelMinded " .. lm.version)
    lm.notify("Hello, World!")
    lm.add_menu_item("Spawn a ring of cubes", spawn_ring)
end

-- Draws this mod's controls in the Mods panel.
function on_ui()
    lm.ui.text("Hello from Lua! Click the button to place a cube outside the building.")
    if lm.ui.button("Spawn Hello Cube") then
        spawned = spawned + 1
        local cube = lm.scene.spawn("Cube", {
            name = "Hello_Cube_" .. spawned,
            position = { (spawned - 1) * spacing, -14, 0.5 },
        })
        lm.scene.select(cube)
        print("Spawned " .. lm.scene.name(cube))
    end
    lm.ui.same_line()
    lm.ui.text(spawned .. " spawned")
    spacing = lm.ui.slider("Spacing", spacing, 1.0, 5.0)
end
