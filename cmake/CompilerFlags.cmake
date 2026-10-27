# Flags shared by every first-party target. Link `project_flags` to opt in.
#
# Determinism (DESIGN.md 4.1): no fast-math and no FMA contraction, so float
# results don't depend on how the compiler chose to fuse operations.

add_library(project_flags INTERFACE)
add_library(project::flags ALIAS project_flags)

# Windows threads get a 1 MB (MSVC) or 2 MB (MinGW) main stack by default;
# deep script recursion and the parser's tests need what Linux gives (8 MB).
if(MSVC)
    # CMake's MSVC defaults can be absent; the engine uses exceptions, so ask for /EHsc.
    string(APPEND CMAKE_CXX_FLAGS " /EHsc")
    # /fp:precise doesn't contract to FMA since VS 2022 (/fp:contract is opt-in);
    # tests/cuervo/platform_tests.cpp checks it.
    target_compile_options(project_flags INTERFACE /W4 /permissive- /fp:precise /utf-8 /Gy)
    target_link_options(project_flags INTERFACE /OPT:REF /OPT:ICF /STACK:8388608)
else()
    target_compile_options(project_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        -ffp-contract=off
        -fno-fast-math
        -ffunction-sections
        -fdata-sections
    )
    if(APPLE)
        target_link_options(project_flags INTERFACE -Wl,-dead_strip)
    elseif(WIN32 AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # MinGW-w64 (GCC or Clang): GNU ld or lld in its GNU mode. -static links
        # libgcc, libstdc++ and winpthread in, so programs and exported players
        # don't need MinGW's DLLs on PATH (the C runtime is the system UCRT).
        target_link_options(project_flags INTERFACE -Wl,--gc-sections -Wl,--stack,8388608 -static)
    elseif(WIN32)
        # Clang targeting the MSVC runtime: lld-link.
        target_link_options(project_flags INTERFACE -Wl,/OPT:REF -Wl,/OPT:ICF -Wl,/STACK:8388608)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            execute_process(
                COMMAND ${CMAKE_CXX_COMPILER} --rtlib=compiler-rt --print-libgcc-file-name
                OUTPUT_VARIABLE _cuervo_builtins
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(EXISTS "${_cuervo_builtins}")
                target_link_libraries(project_flags INTERFACE "${_cuervo_builtins}")
            endif()
        endif()
    else()
        target_link_options(project_flags INTERFACE -Wl,--gc-sections)
    endif()
endif()

target_include_directories(project_flags INTERFACE "${PROJECT_SOURCE_DIR}")

# Explicit SIMD (DESIGN.md 4.15). One ISA per build, for every first-party
# target (inline Vec code must see the same ISA everywhere):
#   sse2     x86-64 baseline: every x86-64 CPU has it (the default there,
#            safe for distribution)
#   avx2     8-wide paths; needs a Haswell/Zen or newer CPU to run
#   simd128  WebAssembly SIMD (the web build's default, -msimd128)
#   none     scalar code only
# FMA stays off (no -mfma; -ffp-contract=off above), so every choice gives
# bit-identical simulation results.
include(CheckCXXCompilerFlag)
if(EMSCRIPTEN)
    set(_cuervo_simd_default simd128)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_cuervo_simd_default sse2)
else()
    set(_cuervo_simd_default none)
endif()
set(CUERVO_SIMD "${_cuervo_simd_default}" CACHE STRING "Explicit SIMD paths: avx2, sse2, simd128 (web) or none")
set_property(CACHE CUERVO_SIMD PROPERTY STRINGS avx2 sse2 simd128 none)

if(CUERVO_SIMD STREQUAL "avx2")
    if(MSVC)
        set(_cuervo_avx2_flag /arch:AVX2)
    else()
        set(_cuervo_avx2_flag -mavx2)
    endif()
    check_cxx_compiler_flag(${_cuervo_avx2_flag} CUERVO_COMPILER_HAS_AVX2)
    if(EMSCRIPTEN OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" OR NOT CUERVO_COMPILER_HAS_AVX2)
        message(WARNING "CUERVO_SIMD=avx2: ${_cuervo_avx2_flag} is not available for this target; using ${_cuervo_simd_default}")
        set(CUERVO_SIMD "${_cuervo_simd_default}")
    endif()
endif()
if(CUERVO_SIMD STREQUAL "simd128" AND NOT EMSCRIPTEN)
    message(WARNING "CUERVO_SIMD=simd128 is for the web build; using ${_cuervo_simd_default}")
    set(CUERVO_SIMD "${_cuervo_simd_default}")
endif()
if(CUERVO_SIMD STREQUAL "sse2" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(WARNING "CUERVO_SIMD=sse2 needs x86-64; using ${_cuervo_simd_default}")
    set(CUERVO_SIMD "${_cuervo_simd_default}")
endif()

if(CUERVO_SIMD STREQUAL "avx2")
    target_compile_options(project_flags INTERFACE ${_cuervo_avx2_flag})
    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC bug 54412: unaligned 32-byte stack spills on Win64 (see Box2D in Dependencies.cmake).
        target_compile_options(project_flags INTERFACE -Wa,-muse-unaligned-vector-move)
    endif()
    target_compile_definitions(project_flags INTERFACE CUERVO_SIMD_AVX2=1)
elseif(CUERVO_SIMD STREQUAL "sse2")
    target_compile_definitions(project_flags INTERFACE CUERVO_SIMD_SSE2=1)
elseif(CUERVO_SIMD STREQUAL "simd128")
    target_compile_options(project_flags INTERFACE -msimd128)
    target_link_options(project_flags INTERFACE -msimd128)
    target_compile_definitions(project_flags INTERFACE CUERVO_SIMD_WASM=1)
elseif(NOT CUERVO_SIMD STREQUAL "none")
    message(FATAL_ERROR "CUERVO_SIMD must be avx2, sse2, simd128 or none (got '${CUERVO_SIMD}')")
endif()
message(STATUS "Cuervo SIMD: ${CUERVO_SIMD}")
