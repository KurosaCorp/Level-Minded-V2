cmake_minimum_required(VERSION 3.25)

project(LevelMinded
    VERSION 0.1.0
    DESCRIPTION "Optimized 3D map design tool"
    LANGUAGES CXX)

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

add_executable(LevelMinded WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/App.h
    src/app/DebugOverlay.cpp
    src/app/DebugOverlay.h
    src/core/Clock.h
    src/core/Rect.h
    src/editor/EditorCamera.cpp
    src/editor/EditorCamera.h
    src/editor/Viewport.cpp
    src/editor/Viewport.h
    src/platform/Window.cpp
    src/platform/Window.h
    src/render/GpuDevice.cpp
    src/render/GpuDevice.h
    src/render/LineBatch.cpp
    src/render/LineBatch.h
    src/render/ViewportRenderer.cpp
    src/render/ViewportRenderer.h
    src/render/shaders/Common.hlsli
    src/render/shaders/Grid.hlsl
    src/render/shaders/Lines.hlsl
    res/LevelMinded.manifest
)
# Keep the .hlsl files visible in IDEs without MSBuild trying to compile them.
set_source_files_properties(
    src/render/shaders/Common.hlsli src/render/shaders/Grid.hlsl src/render/shaders/Lines.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)

lm_add_shader(LevelMinded src/render/shaders/Grid.hlsl  VSMain vs_5_0 g_GridVS)
lm_add_shader(LevelMinded src/render/shaders/Grid.hlsl  PSMain ps_5_0 g_GridPS)
lm_add_shader(LevelMinded src/render/shaders/Lines.hlsl VSMain vs_5_0 g_LineVS)
lm_add_shader(LevelMinded src/render/shaders/Lines.hlsl PSMain ps_5_0 g_LinePS)

target_include_directories(LevelMinded PRIVATE src)
target_link_libraries(LevelMinded PRIVATE imgui d3d11 dxgi dwmapi)
target_compile_definitions(LevelMinded PRIVATE
    LM_VERSION_STRING="${PROJECT_VERSION}"
    LM_BUILD_CONFIG="$<CONFIG>")
lm_apply_project_warnings(LevelMinded)

set_target_properties(LevelMinded PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
