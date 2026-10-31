# LevelMinded

Optimized 3D map design tool. C++20, CMake + Ninja, MSVC, Win32 + Direct3D 11, Dear ImGui (docking branch).

## Build

```powershell
.\build.ps1                 # Release (default)
.\build.ps1 -Preset debug   # Debug, D3D debug layer if installed
.\build.ps1 -Preset profile # Release optimizations + PDB
.\build.ps1 -Run            # build, then launch
```

`build.ps1` loads the MSVC environment itself (via vswhere + vcvars64), so no Developer Prompt is needed.
Output: `build/<preset>/bin/LevelMinded.exe`. Dear ImGui is fetched by CMake (pinned tag + SHA256 in `cmake/Dependencies.cmake`).

## Layout

- `src/platform/Window.*` - Win32 window, message pump, DPI handling
- `src/render/GpuDevice.*` - D3D11 device, flip-model swap chain with frame-latency waitable, depth buffer
- `src/render/ViewportRenderer.*` - per-viewport constants/states; infinite grid pass; flushes the line batch
- `src/render/LineBatch.*` - all debug lines of a frame in one dynamic VB and one draw call
- `src/render/shaders/*.hlsl` - compiled to bytecode headers at build time (`cmake/Shaders.cmake`, fxc)
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
