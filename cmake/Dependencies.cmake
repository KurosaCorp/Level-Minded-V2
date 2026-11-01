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

# simdjson (map loading): the official single-header amalgamation, pinned by version and checksum.
# Downloaded directly (the full source archive is large); dual-licensed Apache-2.0 / MIT, used under MIT.
set(LM_SIMDJSON_VERSION "5.0.5")
set(LM_SIMDJSON_DIR "${FETCHCONTENT_BASE_DIR}/simdjson-${LM_SIMDJSON_VERSION}")
function(lm_download_checked url destination sha256)
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" existing)
        if(existing STREQUAL sha256)
            return()
        endif()
    endif()
    file(DOWNLOAD "${url}" "${destination}" STATUS status TLS_VERIFY ON)
    list(GET status 0 code)
    if(EXISTS "${destination}")
        file(SHA256 "${destination}" actual)
    endif()
    if(NOT code EQUAL 0 OR NOT actual STREQUAL sha256)
        file(REMOVE "${destination}")
        message(FATAL_ERROR "Download failed or checksum mismatch: ${url} (${status})")
    endif()
endfunction()
set(LM_SIMDJSON_URL "https://raw.githubusercontent.com/simdjson/simdjson/v${LM_SIMDJSON_VERSION}")
lm_download_checked("${LM_SIMDJSON_URL}/singleheader/simdjson.h" "${LM_SIMDJSON_DIR}/simdjson.h"
    869e8a4ff285cec36bc79d4a671a433158391a4f1898bb1c3b1f6e654b9547ae)
lm_download_checked("${LM_SIMDJSON_URL}/singleheader/simdjson.cpp" "${LM_SIMDJSON_DIR}/simdjson.cpp"
    97d622441079fae86c7c342508880168bd7bf9e3342b1f4c565077da0155ca79)
lm_download_checked("${LM_SIMDJSON_URL}/LICENSE-MIT" "${LM_SIMDJSON_DIR}/LICENSE-MIT"
    9ed0a34979f22fc33fc13d942233f22d44906c236d876bd05fadf18cb5abf7da)

add_library(lm_simdjson STATIC "${LM_SIMDJSON_DIR}/simdjson.cpp")
target_include_directories(lm_simdjson SYSTEM PUBLIC "${LM_SIMDJSON_DIR}")
# Error codes instead of exceptions; no deprecated API. Must match in every user of the header.
# Runtime CPU dispatch keeps only the AVX2 parser and the portable fallback (every x86-64 CPU is
# covered); the AVX-512 and SSE4.2 variants would add ~200 KB to the exe for no gain at our file sizes.
target_compile_definitions(lm_simdjson PUBLIC SIMDJSON_EXCEPTIONS=0 SIMDJSON_DISABLE_DEPRECATED_API=1
    SIMDJSON_IMPLEMENTATION_ICELAKE=0 SIMDJSON_IMPLEMENTATION_WESTMERE=0)

# Lua 5.5 (mods): official source release, pinned by checksum, compiled as C++ so script errors are
# C++ exceptions (destructors in the binding code run) instead of longjmp. Only the core and the
# safe libraries are built: no io, os, debug or package (no files, processes or native libraries
# for mods), and not the standalone interpreter/compiler.
FetchContent_Declare(lua
    URL      "https://www.lua.org/ftp/lua-5.5.1.tar.gz"
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR sources-only)
FetchContent_MakeAvailable(lua)
set(LM_LUA_SOURCES)
foreach(name lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem lobject
             lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
    list(APPEND LM_LUA_SOURCES "${lua_SOURCE_DIR}/src/${name}.c")
endforeach()
set_source_files_properties(${LM_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lm_lua STATIC ${LM_LUA_SOURCES})
target_include_directories(lm_lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
# Optimized for size: mod scripts are small and event-driven; full /O2 /Ob3 inlining doubles Lua's size.
target_compile_options(lm_lua PRIVATE /W1)
set_target_properties(lm_lua PROPERTIES LM_OPTIMIZE_FOR_SIZE ON)
