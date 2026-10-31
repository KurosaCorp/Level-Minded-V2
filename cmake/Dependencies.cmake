include(FetchContent)

# Dear ImGui (docking branch), pinned by tag and checksum.
set(LM_IMGUI_VERSION "v1.93.0-docking")
FetchContent_Declare(imgui
    URL      "https://github.com/ocornut/imgui/archive/refs/tags/${LM_IMGUI_VERSION}.tar.gz"
    URL_HASH SHA256=746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD) # no gamepad UI navigation: skip XInput polling every frame
if(LM_SHIP_BUILD)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
endif()

# The DX11 backend compiles its shaders at startup through d3dcompiler_47.dll (ships with Windows 10+).
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# Model loaders (both MIT, single-file C): cgltf for glTF 2.0 (.gltf/.glb), fast_obj for Wavefront OBJ.
# Pinned by tag and checksum; compiled as C with their own default warnings.
# SOURCE_SUBDIR points at a folder that doesn't exist, so only the sources are fetched (fast_obj's own
# CMake project, with its test executable, is not added).
FetchContent_Declare(cgltf
    URL      "https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz"
    URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_Declare(fast_obj
    URL      "https://github.com/thisistherk/fast_obj/archive/refs/tags/v1.3.tar.gz"
    URL_HASH SHA256=f0f175edbe60db2099b3d8266ff0085b21862a17d9a1dc00fd195258c5b622b1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(cgltf fast_obj)

set(LM_LOADERS_IMPL "${CMAKE_BINARY_DIR}/generated/cgltf_impl.c")
# file(CONFIGURE) rewrites it only when the content changes, so reconfiguring doesn't force a rebuild.
file(CONFIGURE OUTPUT "${LM_LOADERS_IMPL}" CONTENT "#define CGLTF_IMPLEMENTATION\n#include \"cgltf.h\"\n" @ONLY)
add_library(lm_loaders STATIC "${LM_LOADERS_IMPL}" "${fast_obj_SOURCE_DIR}/fast_obj.c")
target_include_directories(lm_loaders SYSTEM PUBLIC "${cgltf_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}")
target_compile_definitions(lm_loaders PRIVATE _CRT_SECURE_NO_WARNINGS)
