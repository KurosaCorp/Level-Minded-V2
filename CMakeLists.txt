cmake_minimum_required(VERSION 3.25)

project(LevelMinded
    VERSION 2.0.0
    DESCRIPTION "Optimized 3D map design tool"
    LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "LevelMinded currently targets Windows with MSVC (Win32 + Direct3D 11).")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(BuildSettings)
include(Dependencies)
include(Shaders)

# Data model and CPU-side geometry (no UI or GPU dependencies), shared by the editor and the tests.
add_library(levelminded_core STATIC
    src/core/MeshId.h
    src/mods/ModRuntime.cpp
    src/mods/ModRuntime.h
    src/import/ModelImporter.cpp
    src/import/ModelImporter.h
    src/render/MeshGeometry.cpp
    src/render/MeshGeometry.h
    src/render/MeshLibrary.cpp
    src/render/MeshLibrary.h
    src/scene/Commands.cpp
    src/scene/Commands.h
    src/scene/ItemCatalog.cpp
    src/scene/ItemCatalog.h
    src/scene/MapFile.cpp
    src/scene/MapFile.h
    src/scene/MockLevel.cpp
    src/scene/MockLevel.h
    src/scene/Picking.cpp
    src/scene/Picking.h
    src/scene/SceneTree.cpp
    src/scene/SceneTree.h
    src/scene/Transform.h
)
target_include_directories(levelminded_core PUBLIC src)
target_link_libraries(levelminded_core PRIVATE lm_loaders lm_simdjson lm_lua)
lm_apply_project_warnings(levelminded_core)

# Version-stamped resources, generated from res/*.in.
set(LM_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
if(LM_ENABLE_AVX2)
    set(LM_CPU_REQUIREMENT "A 64-bit CPU with AVX2 (Intel Haswell / AMD Excavator, 2013, or newer)")
else()
    set(LM_CPU_REQUIREMENT "Any 64-bit x86 CPU")
endif()
configure_file(res/LevelMinded.manifest.in "${LM_GENERATED_DIR}/LevelMinded.manifest" @ONLY)
configure_file(res/LevelMinded.rc.in "${LM_GENERATED_DIR}/LevelMinded.rc" @ONLY)
configure_file(res/README.txt.in "${LM_GENERATED_DIR}/README.txt" @ONLY NEWLINE_STYLE CRLF)
# "2.0" for the package folder name (read by build_release.bat).
file(WRITE "${CMAKE_BINARY_DIR}/package_version.txt" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

add_executable(LevelMinded WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/App.h
    src/app/DebugOverlay.cpp
    src/app/DebugOverlay.h
    src/core/AllocationCounter.cpp
    src/core/AllocationCounter.h
    src/core/Clock.h
    src/core/Profiler.h
    src/core/Rect.h
    src/editor/AssetPanel.cpp
    src/editor/AssetPanel.h
    src/editor/EditorCamera.cpp
    src/editor/EditorCamera.h
    src/editor/HierarchyPanel.cpp
    src/editor/HierarchyPanel.h
    src/editor/ModsPanel.cpp
    src/editor/ModsPanel.h
    src/editor/NodeStyle.h
    src/editor/TransformGizmo.cpp
    src/editor/TransformGizmo.h
    src/editor/Viewport.cpp
    src/editor/Viewport.h
    src/platform/Window.cpp
    src/platform/Window.h
    src/render/GpuDevice.cpp
    src/render/GpuDevice.h
    src/render/LineBatch.cpp
    src/render/LineBatch.h
    src/render/StateCache.h
    src/render/ViewportRenderer.cpp
    src/render/ViewportRenderer.h
    src/render/shaders/Common.hlsli
    src/render/shaders/Grid.hlsl
    src/render/shaders/Lines.hlsl
    src/render/shaders/Mesh.hlsl
    "${LM_GENERATED_DIR}/LevelMinded.manifest"
    "${LM_GENERATED_DIR}/LevelMinded.rc"
)
# Keep the .hlsl files visible in IDEs without MSBuild trying to compile them.
set_source_files_properties(
    src/render/shaders/Common.hlsli src/render/shaders/Grid.hlsl src/render/shaders/Lines.hlsl
    src/render/shaders/Mesh.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)

lm_add_shader(LevelMinded src/render/shaders/Grid.hlsl  VSMain vs_5_0 g_GridVS)
lm_add_shader(LevelMinded src/render/shaders/Grid.hlsl  PSMain ps_5_0 g_GridPS)
lm_add_shader(LevelMinded src/render/shaders/Lines.hlsl VSMain vs_5_0 g_LineVS)
lm_add_shader(LevelMinded src/render/shaders/Lines.hlsl PSMain ps_5_0 g_LinePS)
lm_add_shader(LevelMinded src/render/shaders/Mesh.hlsl  VSMain vs_5_0 g_MeshVS)
lm_add_shader(LevelMinded src/render/shaders/Mesh.hlsl  PSMain ps_5_0 g_MeshPS)
lm_add_shader(LevelMinded src/render/shaders/Mesh.hlsl  WirePSMain ps_5_0 g_WirePS)

target_include_directories(LevelMinded PRIVATE src)
target_link_libraries(LevelMinded PRIVATE levelminded_core imgui d3d11 dxgi dwmapi shell32 comdlg32)

# Sample mods next to the development exe (bin/mods), so they load when running from the build tree.
file(GLOB LM_SAMPLE_MODS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/examples/mods/*.lua")
add_custom_command(TARGET LevelMinded POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LevelMinded>/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LM_SAMPLE_MODS} "$<TARGET_FILE_DIR:LevelMinded>/mods"
    COMMENT "Copying sample mods")
target_compile_definitions(LevelMinded PRIVATE
    LM_VERSION_STRING="${PROJECT_VERSION}"
    LM_BUILD_CONFIG="$<IF:$<BOOL:${LM_SHIP_BUILD}>,Release build,$<CONFIG>>")
lm_apply_project_warnings(LevelMinded)

set_target_properties(LevelMinded PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Tests (console app, run with `ctest` or `.\build.ps1 -Test`)
enable_testing()
add_executable(LevelMindedTests tests/SceneTreeTests.cpp)
target_link_libraries(LevelMindedTests PRIVATE levelminded_core)
target_compile_definitions(LevelMindedTests PRIVATE LM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
lm_apply_project_warnings(LevelMindedTests)
set_target_properties(LevelMindedTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_test(NAME SceneTree COMMAND LevelMindedTests)

# ---- Release package (`cmake --install <build dir> --prefix <folder>`, used by build_release.bat).
# The exe has no runtime dependencies beyond Windows itself (static CRT; D3D11, DXGI, d3dcompiler_47
# ship with Windows 10+), and shaders are compiled into it, so it is the only binary in the package.
install(TARGETS LevelMinded RUNTIME DESTINATION .)
install(FILES "${LM_GENERATED_DIR}/README.txt" DESTINATION .)
install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt" DESTINATION . RENAME THIRD_PARTY_LICENSE_DearImGui.txt)
install(FILES "${cgltf_SOURCE_DIR}/LICENSE" DESTINATION . RENAME THIRD_PARTY_LICENSE_cgltf.txt)
install(FILES "${fast_obj_SOURCE_DIR}/LICENSE" DESTINATION . RENAME THIRD_PARTY_LICENSE_fast_obj.txt)
install(FILES "${LM_SIMDJSON_DIR}/LICENSE-MIT" DESTINATION . RENAME THIRD_PARTY_LICENSE_simdjson.txt)
install(FILES res/licenses/Lua.txt DESTINATION . RENAME THIRD_PARTY_LICENSE_Lua.txt)
install(FILES examples/mods/hello_world.lua DESTINATION mods)
