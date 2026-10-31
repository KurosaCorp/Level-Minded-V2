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

# The DX11 backend compiles its shaders at startup through d3dcompiler_47.dll (ships with Windows 10+).
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)
