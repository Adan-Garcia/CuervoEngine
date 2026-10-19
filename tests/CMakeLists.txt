set(CUERVO_TEST_SOURCES
    doctest_main.cpp
    cuervo/vec_tests.cpp
    cuervo/dmath_tests.cpp
    cuervo/rng_tests.cpp
    cuervo/math_tests.cpp
    cuervo/fixed_timestep_tests.cpp
    cuervo/name_tests.cpp
    cuervo/input_tests.cpp
    cuervo/camera_tests.cpp
    cuervo/sdf_batcher_tests.cpp
    cuervo/vector_tests.cpp
    cuervo/svg_tests.cpp
    cuervo/text_tests.cpp
    cuervo/script_tests.cpp
    cuervo/script_memory_tests.cpp
    cuervo/scene_tests.cpp
    cuervo/tier_tests.cpp
    cuervo/jobs_tests.cpp
    cuervo/simd_tests.cpp
    cuervo/session_tests.cpp
    cuervo/pack_tests.cpp
    cuervo/export_tests.cpp
    cuervo/lsp_tests.cpp
    cuervo/feature_tests.cpp
    cuervo/api_docs_tests.cpp
    cuervo/tutorial_tests.cpp
    cuervo/player_tests.cpp
    cuervo/profiler_tests.cpp
    cuervo/brand_tests.cpp
    cuervo/node_tests.cpp
    cuervo/hierarchy_tests.cpp
    cuervo/eula_tests.cpp
    ${PROJECT_SOURCE_DIR}/pluma/repl/eula_gate.cpp
)

if(CUERVO_FEATURE_AUDIO)
    list(APPEND CUERVO_TEST_SOURCES cuervo/audio_tests.cpp)
endif()
if(CUERVO_FEATURE_COLLISION)
    list(APPEND CUERVO_TEST_SOURCES cuervo/collision_tests.cpp)
endif()
if(CUERVO_FEATURE_PARTICLES)
    list(APPEND CUERVO_TEST_SOURCES cuervo/particle_tests.cpp)
endif()
if(CUERVO_FEATURE_UI)
    list(APPEND CUERVO_TEST_SOURCES cuervo/ui_tests.cpp)
endif()

add_executable(cuervo_tests ${CUERVO_TEST_SOURCES})
target_link_libraries(cuervo_tests PRIVATE cuervo cuervo_export cuervo_api_docs doctest)
target_include_directories(cuervo_tests PRIVATE ${PROJECT_SOURCE_DIR}/pluma/repl)
target_compile_definitions(cuervo_tests PRIVATE CUERVO_EULA_VERSION="${CUERVO_EULA_VERSION}" CUERVO_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
    CUERVO_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
add_test(NAME cuervo_tests COMMAND cuervo_tests)

add_executable(pluma_tests
    doctest_main.cpp
    pluma/lexer_tests.cpp
    pluma/parser_expr_tests.cpp
    pluma/parser_stmt_tests.cpp
    pluma/parser_decl_tests.cpp
    pluma/parser_example_tests.cpp
    pluma/checker_tests.cpp
    pluma/vm_tests.cpp
    pluma/vec_tests.cpp
    pluma/coroutine_tests.cpp
    pluma/native_tests.cpp
    pluma/reload_tests.cpp
    pluma/export_tests.cpp
    pluma/memory_tests.cpp
    pluma/aot_tests.cpp
    pluma/library_tests.cpp
    pluma/math_tests.cpp
    pluma/format_tests.cpp
    pluma/lsp_tests.cpp
    pluma/debug_tests.cpp
    pluma/dap_tests.cpp
    pluma/profiler_tests.cpp
)
target_link_libraries(pluma_tests PRIVATE pluma doctest)
add_test(NAME pluma_tests COMMAND pluma_tests)

# Backend parity through the frame-capture path (tests/rhi_parity.cmake).
if(TARGET rhi_parity AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    add_test(NAME rhi_parity COMMAND ${CMAKE_COMMAND}
        -DRHI_PARITY=$<TARGET_FILE:rhi_parity>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DSHMUP=${PROJECT_SOURCE_DIR}/examples/shmup
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rhi_parity
        -P ${CMAKE_CURRENT_SOURCE_DIR}/rhi_parity.cmake)
    set_tests_properties(rhi_parity PROPERTIES TIMEOUT 300)
endif()

# docs/API.md is generated from the engine prelude (`cuervo-tools docs`), so
# it can't drift from what scripts can call. Only a build with every optional
# prelude compiled in generates the whole reference.
if(TARGET cuervo_tools AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES)
    add_test(NAME api_docs_up_to_date COMMAND cuervo_tools docs --check ${PROJECT_SOURCE_DIR}/docs/API.md)
endif()

# `cuervo --game --profile` logs the engine zones and the script functions at
# exit (DESIGN.md 5.14 "Clarified (Phase 8 -- Profiler)"), headless on GL.
if(TARGET runtime AND CUERVO_PROFILING AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    add_test(NAME profile_shmup COMMAND $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/shmup
        --backend gl --no-audio --quit-after 1 --profile)
    if(NOT CMAKE_HOST_WIN32)
        set_tests_properties(profile_shmup PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    endif()
    set_tests_properties(profile_shmup PROPERTIES TIMEOUT 60
        PASS_REGULAR_EXPRESSION "profile \\|     simulation .*profile \\|           update .*profile \\| script time: [0-9.]+ ms in [1-9][0-9]* function.*Game\\.draw \\(scripts/game\\.plm")
endif()

# Every pinned dependency has its license text and notice (THIRD_PARTY_NOTICES.md).
add_test(NAME third_party_notices COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/third_party_notices.cmake)

# The migration report CLI on the shipped example (exit 0 = fits its tier).
add_test(NAME tier_check_shmup COMMAND cuervo_tools tier check ${PROJECT_SOURCE_DIR}/examples/shmup)

# The editor's core, headless (editor/, DESIGN.md 4.13).
if(TARGET editor_core)
    add_executable(editor_tests doctest_main.cpp editor/editor_tests.cpp editor/editor_6b_tests.cpp editor/editor_83_tests.cpp)
    target_link_libraries(editor_tests PRIVATE editor_core doctest)
    target_compile_definitions(editor_tests PRIVATE CUERVO_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
        CUERVO_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf")
    add_test(NAME editor_tests COMMAND editor_tests)
endif()

# A level built only from editor operations, saved by cuervo-editor and run
# by the runtime (tests/editor_level.cmake).
if(TARGET editor AND TARGET runtime)
    add_test(NAME editor_level COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_level
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_level.cmake)
    set_tests_properties(editor_level PROPERTIES TIMEOUT 120)
endif()

# Milestone 8.2's done-when: every built-in node in one level built from
# editor operations, round-tripped through scene files to the same state hash
# (tests/node_level.cmake). The editor requires every feature.
if(TARGET editor AND TARGET runtime AND TARGET cuervo_tools)
    add_test(NAME node_level COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/node_level
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/node_level
        -P ${CMAKE_CURRENT_SOURCE_DIR}/node_level.cmake)
    set_tests_properties(node_level PROPERTIES TIMEOUT 180)
endif()

# Play-in-editor and replays through the shipped binaries: the runtime records
# a run, the editor verifies it on 1 and 8 threads, rejects a drifted project,
# and reaches the runtime's state hash when it plays the same input
# (tests/editor_replay.cmake).
if(TARGET editor AND TARGET runtime)
    add_test(NAME editor_replay COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_replay
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_replay.cmake)
    set_tests_properties(editor_replay PROPERTIES TIMEOUT 180)
endif()

# The release path end to end: export the shmup for Linux twice (identical
# game.pak), ship only dist/, run it from elsewhere and compare the state hash
# with `cuervo --game examples/shmup` (tests/export_shmup.cmake, docs/EXPORT.md).
if(TARGET runtime AND TARGET cuervo_tools AND NOT EMSCRIPTEN AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    add_test(NAME export_shmup COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/export_shmup
        -DEXPECT=a3e57e79964421d3
        -P ${CMAKE_CURRENT_SOURCE_DIR}/export_shmup.cmake)
    set_tests_properties(export_shmup PROPERTIES TIMEOUT 180)
endif()

# The debug adapter end to end (`cuervo --dap`, runtime/dap_cli.hpp): a DAP
# client script drives breakpoints, stepping and inspection on tests/dap_game.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(TARGET runtime AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
    add_test(NAME dap_session
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/dap_session.py
                $<TARGET_FILE:runtime> ${CMAKE_CURRENT_SOURCE_DIR}/dap_game)
    set_tests_properties(dap_session PROPERTIES TIMEOUT 120)
endif()

# assets/brand/ only changes through tools/sync_brand.py (DESIGN.md 4.16): its
# files match the hashes the script recorded in SOURCE.txt. Needs no design kit.
if(Python3_Interpreter_FOUND)
    add_test(NAME brand_assets
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/sync_brand.py --verify)
endif()

# Per-export stripped players end to end (milestone 7.5, tests/export_stripped.cmake):
# export examples/tictak with --engine stripped, ship only dist/, and compare
# it with the dev run. Off by default because the first run configures and
# builds a Release engine; the player cache stays in the build tree.
option(CUERVO_SLOW_TESTS "Add tests that build engines (stripped-player exports)" OFF)
if(CUERVO_SLOW_TESTS AND TARGET runtime AND TARGET cuervo_tools AND NOT EMSCRIPTEN)
    add_test(NAME export_stripped_tictak COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/tictak
        -DNAME=tictak
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/export_stripped_tictak
        -DPLAYER_CACHE=${CMAKE_CURRENT_BINARY_DIR}/player_cache
        -P ${CMAKE_CURRENT_SOURCE_DIR}/export_stripped.cmake)
    set_tests_properties(export_stripped_tictak PROPERTIES TIMEOUT 1800)
endif()

# The EULA gate through the shipped binaries (tests/eula.cmake).
if(TARGET runtime AND TARGET cuervo_tools AND NOT EMSCRIPTEN)
    add_test(NAME eula_gate COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools> -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/tictak -DVERSION=${CUERVO_EULA_VERSION}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/eula_gate
        -P ${CMAKE_CURRENT_SOURCE_DIR}/eula.cmake)
    set_tests_properties(eula_gate PROPERTIES TIMEOUT 180)
endif()

# Every test that runs a program (cuervo, cuervo-tools, cuervo-editor, plm) accepts the EULA the way CI
# does (DESIGN.md 4.16 "EULA agreement"); tests/eula.cmake checks the gate itself with this unset.
get_property(_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _all_tests)
    if(_t STREQUAL "eula_gate")
        continue()
    endif()
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "CUERVO_ACCEPT_EULA=${CUERVO_EULA_VERSION}")
endforeach()

# The editor's style against ImGui's own defaults (theme.cpp only; no window).
if(TARGET editor AND TARGET imgui)
    add_executable(editor_ui_tests doctest_main.cpp editor/editor_ui_tests.cpp ${PROJECT_SOURCE_DIR}/editor/theme.cpp)
    target_link_libraries(editor_ui_tests PRIVATE cuervo imgui doctest SDL3::SDL3)
    target_compile_definitions(editor_ui_tests PRIVATE
        CUERVO_FONT_UI="" CUERVO_FONT_MONO="" CUERVO_FONT_DISPLAY="")
    add_test(NAME editor_ui_tests COMMAND editor_ui_tests)
endif()
