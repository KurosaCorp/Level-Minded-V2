# Compiles HLSL to bytecode headers at build time with fxc (Windows SDK), so shader errors fail
# the build and the app does no shader compilation at startup.
#
#   lm_add_shader(<target> <file.hlsl> <entry> <profile> <variable>)
#
# Generates ${CMAKE_BINARY_DIR}/shaders/<variable>.h containing `const BYTE <variable>[]`.

# Fallback when not run from a VS developer environment: newest SDK under Windows Kits.
set(_lm_pf86 "ProgramFiles(x86)")
file(GLOB _lm_sdk_bins "$ENV{${_lm_pf86}}/Windows Kits/10/bin/10.*")
list(SORT _lm_sdk_bins COMPARE NATURAL ORDER DESCENDING)
list(TRANSFORM _lm_sdk_bins APPEND "/x64")

find_program(LM_FXC fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
    PATHS ${_lm_sdk_bins}
    REQUIRED)

set(LM_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${LM_SHADER_OUTPUT_DIR}")

function(lm_add_shader target hlsl entry profile variable)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl}")
    set(output "${LM_SHADER_OUTPUT_DIR}/${variable}.h")
    get_filename_component(sourceDir "${source}" DIRECTORY)
    file(GLOB includes CONFIGURE_DEPENDS "${sourceDir}/*.hlsli")

    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${LM_FXC}" /nologo /T ${profile} /E ${entry} /Vn ${variable} /Fh "${output}" /WX
                "$<IF:$<CONFIG:Debug>,/Od;/Zi,/O3;/Qstrip_reflect;/Qstrip_debug>"
                "${source}"
        DEPENDS "${source}" ${includes}
        COMMENT "Compiling shader ${hlsl} (${entry}, ${profile})"
        COMMAND_EXPAND_LISTS
        VERBATIM)

    target_sources(${target} PRIVATE "${output}")
    target_include_directories(${target} PRIVATE "${LM_SHADER_OUTPUT_DIR}")
endfunction()
