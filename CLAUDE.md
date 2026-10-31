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

## Release package

```bat
build_release.bat
```

Clean "dist" preset build (Release + `LM_SHIP_BUILD`: ImGui demo/debug tools compiled out), runs the tests,
verifies the exe (GUI subsystem, no PDB record, only Windows system DLLs), then `cmake --install`s it into
`dist\LevelMinded_v<major>.<minor>\` (exe, README, Dear ImGui license) and zips it. The version comes from
`project(VERSION)` in `CMakeLists.txt`; README, manifest and the version resource are generated from `res/*.in`.
The exe must stay self-contained: static CRT, shaders compiled in, no files loaded at startup.

## Profiling

```powershell
build\release\bin\LevelMinded.exe --stress 100000 --bench 300 --bench-out bench.txt
```

`--stress N` adds N items; `--bench N` runs a scripted camera orbit (N frames static scene, N frames
with a mass-select toggle every frame, then N frames moving one item every frame), writes per-zone CPU times, allocations/frame, GPU upload/frame,
state changes and memory to the report, and exits. The F1 overlay shows the same numbers live
("Profiler" section). Run it before and after performance work and compare.
`--import PATH` (repeatable) imports a model at startup through the same path as drag & drop; `--map PATH` loads a map.
Output: `build/<preset>/bin/LevelMinded.exe`. Dear ImGui, cgltf and fast_obj are fetched by CMake (pinned tag + SHA256
in `cmake/Dependencies.cmake`); the two loaders build as the C library `lm_loaders` (the project enables C for it).
simdjson's single-header amalgamation is downloaded with checksums (`lm_download_checked`) into `lm_simdjson`
(no exceptions, AVX2 + fallback kernels only); only `src/scene/MapFile.cpp` includes the 10 MB header.

## Layout

- `src/platform/Window.*` - Win32 window, message pump, DPI handling
- `src/render/GpuDevice.*` - D3D11 device, flip-model swap chain with frame-latency waitable, depth buffer
- `src/render/ViewportRenderer.*` - per-viewport frame: instanced meshes, grid, depth-tested lines, overlay lines
- `src/render/MeshGeometry.*` - built-in procedural meshes (cube, sphere, cylinder, spawn); `MeshVertex` (pos, normal, RGBA8)
- `src/render/MeshLibrary.*` - all meshes (built-in + imported) in one append-only vertex/index array; `MeshId` indexes it
- `src/import/ModelImporter.*` - glTF (cgltf) / OBJ (fast_obj) -> one normalized Z-up mesh + half extents
- `src/render/LineBatch.*` - lines in one dynamic VB and one draw call; uploaded only when changed
- `src/render/StateCache.h` - filters redundant D3D11 state calls within a frame
- `src/core/Profiler.h`, `src/core/AllocationCounter.*` - per-zone CPU timers, heap/ImGui allocation counters
- `src/render/shaders/*.hlsl` - compiled to bytecode headers at build time (`cmake/Shaders.cmake`, fxc)
- `src/core/MeshId.h` - built-in mesh ids shared by scene and renderer
- `src/scene/SceneTree.*` - hierarchy of groups/items with per-item transform + half extents + mesh
- `src/scene/Transform.h` - TRS transform, `ItemMatrix`, gizmo operations (`Translated`, `RotatedAbout`, `ScaledAbout`)
- `src/scene/Picking.*` - `ScenePicker`: ray vs exact mesh triangles of visible items (click selection)
- `src/scene/MapFile.*` - .lmmap save (hand-written JSON) / load (simdjson, two-phase, all-or-nothing)
- `src/scene/ItemCatalog.*` - built-in item types, `AssetCatalog` (built-in + imported items), `SpawnCatalogItem`
- `src/scene/MockLevel.*` - sample level used until real level loading exists
- `src/editor/HierarchyPanel.*` - left dock panel: tree view, selection, eye toggles, drag & drop, active group
- `src/editor/AssetPanel.*` - right dock panel: catalog tiles, import status line; click spawns into the active group
- `src/editor/NodeStyle.h` - per-kind colors shared by panels and viewport
- `tests/SceneTreeTests.cpp` - unit tests (tree, catalog, mesh winding, transforms, picking, import, maps) + benchmark (`LevelMindedTests`)
- Static lib `levelminded_core` = `src/scene/*` + `src/import/*` + `MeshGeometry`/`MeshLibrary` (no UI/GPU deps)
- `src/editor/EditorCamera.*` - orbit/pan/dolly/fly camera math
- `src/editor/Viewport.*` - camera input, click selection, gizmo toolbar + shortcuts, keyboard focus, orientation axes
- `src/editor/TransformGizmo.*` - move/rotate/scale handles: hit testing, drag math, ImGui drawing
- `src/app/App.*` - init/shutdown, main loop, ImGui + dockspace setup, File menu, import queue (drop / dialog / --import)
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
- Item placement: `Transform` = position (pivot = box center), unit quaternion, scale (1 = catalog size).
  World = scale * halfExtents, then rotate, then translate (`ItemMatrix`). Groups have no transform.
  Quaternions follow DirectXMath: `XMQuaternionMultiply(a, b)` applies a, then b; the shader's
  `QuatRotate` matches `XMVector3Rotate`. Mesh instances are 48 bytes (pos, quat, size, color, flags).
- The transform gizmo recomputes every target from the transforms captured at drag start (no drift;
  Esc restores them). Targets = selected, effectively visible items; groups transform as their items.
- Meshes: `MeshId` < `kBuiltinMeshCount` are the built-ins, the rest are imported (`IsImportedMesh`). The
  `MeshLibrary` only ever appends (max 4096 meshes); `ViewportRenderer` mirrors it in one DEFAULT vertex
  buffer + one R32 index buffer that grow by doubling and upload only the new range (`SyncMeshes`).
  Imported vertices carry the material color; their instances use white so it shows unchanged.
- Imported models are converted to the built-in mesh contract: Z-up ((x, y, z) -> (x, -z, y) from Y-up),
  glTF node transforms baked in (winding flipped for mirrored nodes), normalized into `[-1, 1]^3` with
  the real size as half extents (1 unit = 1 m), CCW from outside. `tests/` builds glTF/GLB/OBJ files
  on the fly to check all of this; extend those tests when the importer changes.
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
- Steady-state frames must make **zero heap allocations** (overlay "Allocs" line / bench report).
  Use retained buffers and fixed-size scratch, not per-frame `std::string`/`std::vector` construction.
- The 3D scene is retained: `Viewport::SubmitScene` rebuilds it only when `SceneTree::RenderVersion()`
  changes, and the renderer uploads it once. Structural, visibility and selection changes must bump
  `RenderVersion()`; anything that changes hierarchy rows must bump `LayoutVersion()`.
- Transforms change only through `SceneTree::SetTransform`, which bumps `TransformVersion()` and logs
  the item in `TransformChanges()` instead of `RenderVersion()`. `SubmitScene` then patches just those
  instances (`ViewportRenderer::UpdateInstance`, partial `UpdateSubresource`), so dragging one item in
  a 100k-item level uploads 48 bytes, not the scene. Keep it that way: never rebuild for a move.
- Scene names live in one shared character buffer (`SceneTree::NameAt` returns a `string_view` that is
  only valid until the next edit). Don't hold it across edits.
- Bind pipeline state in `ViewportRenderer` through `m_state` (the `StateCache`), never directly.
- Map format: bump `kMapFormatVersion` for incompatible changes and keep loading older versions. Floats are
  written with `std::to_chars` (shortest) and read with `from_chars(float)` from the raw token, so maps
  round-trip bit for bit; the tests require load + save to reproduce the file byte for byte.
- Anything that a map saves must bump `SceneTree::ContentVersion()` when it changes (drives the `*`
  unsaved-changes marker and prompts). Selection and expand/collapse deliberately don't.
- Loading replaces the scene: `SceneTree::Clear` (frees memory), `MeshLibrary::Reset` (renderer re-uploads
  on `Generation()` change), `AssetCatalog::ResetImported`, then `HierarchyPanel::Reset` / `AssetPanel::Reset`.
  Anything new that caches NodeIds or palette indices needs a reset there too.
- After every modal OS dialog (file dialogs, message boxes) call `App::AfterModalDialog()`: key-ups that
  happen while it is open never reach ImGui, so shortcuts would get stuck otherwise.
- Model files are opened through the loaders' file callbacks with `_wfopen_s` on UTF-8 paths (non-ASCII
  paths must keep working). Modal OS dialogs open between frames (`App::m_openImportDialog`), never
  while an ImGui frame is being built.
