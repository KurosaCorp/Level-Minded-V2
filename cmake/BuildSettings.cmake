# Global compiler/linker settings for MSVC.
#
# Configurations:
#   Debug          - no optimization, full debug info, D3D debug layer.
#   Release        - maximum speed: /O2 /Ob3, whole-program optimization (LTCG),
#                    COMDAT folding, static CRT, no debug info.
#   RelWithDebInfo - same optimizations as Release, plus a PDB for profiling.

option(LM_ENABLE_AVX2 "Compile with /arch:AVX2 (requires a Haswell-or-newer CPU)" ON)

# Static CRT: no vcruntime/msvcp DLL dependency, slightly faster calls into the CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Link-time code generation for optimized configs.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

# Replace CMake's default optimization flags with our own.
set(CMAKE_CXX_FLAGS_RELEASE        "/O2 /Ob3 /DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Ob3 /Zi /DNDEBUG")

add_compile_options(
    /permissive-        # standards conformance
    /Zc:__cplusplus
    /Zc:preprocessor
    /Zc:inline          # drop unreferenced inline functions early
    /utf-8
    /diagnostics:caret
    "$<$<NOT:$<CONFIG:Debug>>:/Oi>"   # intrinsics
    "$<$<NOT:$<CONFIG:Debug>>:/Gy>"   # function-level linking (enables /OPT:ICF)
    "$<$<NOT:$<CONFIG:Debug>>:/Gw>"   # data-level linking
)

if(LM_ENABLE_AVX2)
    add_compile_options(/arch:AVX2)
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00 # Windows 10+
)

add_link_options(
    "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
    "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>"
    "$<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>"
    "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
)

# Strict warnings for our own code only (third-party code stays at defaults).
function(lm_apply_project_warnings target)
    target_compile_options(${target} PRIVATE /W4 /w14265 /w14062)
endfunction()
