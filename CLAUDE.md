# LevelMinded

Optimized 3D map design tool. C++20, CMake + Ninja, MSVC, Win32 + Direct3D 11, Dear ImGui (docking branch).

## Build

```powershell
.\build.ps1                 # Release (default)
.\build.ps1 -Preset debug   # Debug, D3D debug layer if installed
.\build.ps1 -Preset profile # Release optimizations + PDB
.\build.ps1 -Run            # build, then launch
.\build.ps1 -Test           # build, then run unit tests (ctest)
```

`build.ps1` loads the MSVC environment itself (via vswhere + vcvars64), so no Developer Prompt is needed.
Output: `build/<preset>/bin/LevelMinded.exe`. Dear ImGui is fetched by CMake (pinned tag + SHA256 in `cmake/Dependencies.cmake`).

## Layout

- `src/platform/Window.*` - Win32 window, message pump, DPI handling
- `src/render/GpuDevice.*` - D3D11 device, flip-model swap chain with frame-latency waitable, depth buffer
- `src/render/ViewportRenderer.*` - per-viewport frame: instanced meshes, grid, depth-tested lines, overlay lines
- `src/render/MeshGeometry.*` - built-in procedural meshes (cube, sphere, cylinder, spawn) in one VB/IB
- `src/render/LineBatch.*` - all lines of a frame in one dynamic VB and one draw call
- `src/render/shaders/*.hlsl` - compiled to bytecode headers at build time (`cmake/Shaders.cmake`, fxc)
- `src/core/MeshId.h` - built-in mesh ids shared by scene and renderer
- `src/scene/SceneTree.*` - hierarchy of groups/items with per-item bounds + mesh
- `src/scene/ItemCatalog.*` - placeable item types and `SpawnCatalogItem` (base at the origin)
- `src/scene/MockLevel.*` - sample level used until real level loading exists
- `src/editor/HierarchyPanel.*` - left dock panel: tree view, selection, eye toggles, drag & drop, active group
- `src/editor/AssetPanel.*` - right dock panel: catalog tiles; click spawns into the active group
- `src/editor/NodeStyle.h` - per-kind colors shared by panels and viewport
- `tests/SceneTreeTests.cpp` - unit tests (tree, catalog, mesh winding) + 100k-node benchmark (`LevelMindedTests`)
- Static lib `levelminded_core` = `src/scene/*` + `MeshGeometry` (no UI/GPU deps); linked by app and tests
- `src/editor/EditorCamera.*` - orbit/pan/dolly/fly camera math
- `src/editor/Viewport.*` - camera input handling, keyboard focus, orientation gizmo
- `src/app/App.*` - init/shutdown, main loop, ImGui + dockspace setup
- `src/app/DebugOverlay.*` - stats overlay (F1)
- `cmake/BuildSettings.cmake` - compiler/linker flags per configuration

## Conventions

- World space: right-handed, **Z-up**, units = meters. X red, Y green, Z blue.
- Matrices: DirectXMath row-vector convention; HLSL cbuffers declare `row_major` and use `mul(v, M)`.
- Depth: **reversed-Z** with infinite far plane. Clear depth to 0, compare `GREATER_EQUAL`.
- The 3D viewport is the passthru central node of the ImGui dockspace and renders straight into the
  back buffer region of that node (no offscreen target). Future panels dock around it.
- `SceneTree` is one flat array in depth-first order with subtree sizes: a subtree is always the
  contiguous range `[i, SubtreeEnd(i))`. Keep whole-subtree operations as linear scans. Indices change
  on structural edits; hold `NodeId`s across edits. Call `Validate()` in tests after every new kind of edit.
- Visibility: `kVisible` is the node's own toggle; `kEffectivelyVisible` (cached) also requires every
  ancestor visible. Selecting a node selects its subtree; deselecting clears its ancestors.
- UI code must not make structural tree edits while iterating rows; queue them (see `HierarchyPanel`).
- Meshes are modeled in `[-1, 1]^3` and scaled by the item's half extents; triangles are CCW seen from
  outside (the mesh rasterizer uses `FrontCounterClockwise`). `TestMeshWinding` enforces this.
- Draw calls per viewport: one per mesh type in use (instanced) + grid + lines + overlay lines.
  Keep new per-item visuals batched the same way rather than adding per-item draws.
- The "active group" (Hierarchy's last clicked group, or the clicked item's parent) is where new
  content goes; it is marked with an orange bar in the Hierarchy and shown in the Assets panel.
- Viewport keyboard shortcuts use the viewport's own focus (`Viewport::m_hasKeyboardFocus`), not
  `io.WantCaptureKeyboard`, which stays set whenever any ImGui window has nav focus.

## Rules

- The main loop is event-driven: it renders only for ~0.5 s after a window message, then blocks in
  `MsgWaitForMultipleObjectsEx`. Idle CPU must stay ~0%. Never add per-frame polling that would keep it awake.
  Anything that animates without input (e.g. fly mode with a key held) must report it, like
  `Viewport::IsInteracting()`, so `App` keeps rendering.
- Messages that our own frame triggers must not count as activity (see `IsActivityMessage` in `App.cpp`;
  e.g. ImGui's `WindowFromPoint` call sends `WM_NCHITTEST` every frame).
- The D3D11 device is created `SINGLETHREADED`; use it from the main thread only.
