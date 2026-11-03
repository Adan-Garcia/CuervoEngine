set(CUERVO_TEST_SOURCES
    doctest_main.cpp
    cuervo/vec_tests.cpp
    cuervo/net_loopback_tests.cpp
    cuervo/net_lockstep_tests.cpp
    cuervo/net_rollback_tests.cpp
    cuervo/net_udp_tests.cpp
    cuervo/net_websocket_tests.cpp
    cuervo/net_http_tests.cpp
    cuervo/net_script_tests.cpp
    cuervo/net_replay_tests.cpp
    cuervo/dmath_tests.cpp
    cuervo/rng_tests.cpp
    cuervo/math_tests.cpp
    cuervo/fixed_timestep_tests.cpp
    cuervo/name_tests.cpp
    cuervo/input_tests.cpp
    cuervo/camera_tests.cpp
    cuervo/sdf_batcher_tests.cpp
    cuervo/sprite_batcher_tests.cpp
    cuervo/post_fx_tests.cpp
    cuervo/user_shader_tests.cpp
    cuervo/frame_fence_ring_tests.cpp
    cuervo/vector_tests.cpp
    cuervo/svg_tests.cpp
    cuervo/text_tests.cpp
    cuervo/script_tests.cpp
    cuervo/package_tests.cpp
    cuervo/plugin_tests.cpp
    cuervo/registry_tests.cpp
    cuervo/script_memory_tests.cpp
    cuervo/scene_tests.cpp
    cuervo/tier_tests.cpp
    cuervo/webgpu_tests.cpp
    cuervo/support_tests.cpp
    cuervo/jobs_tests.cpp
    cuervo/simd_tests.cpp
    cuervo/session_tests.cpp
    cuervo/snapshot_tests.cpp
    cuervo/pack_tests.cpp
    cuervo/raster_import_tests.cpp
    cuervo/sprite_assets_tests.cpp
    cuervo/sprite_nodes_tests.cpp
    cuervo/export_tests.cpp
    cuervo/lsp_tests.cpp
    cuervo/feature_tests.cpp
    cuervo/api_docs_tests.cpp
    cuervo/cli_table_tests.cpp
    cuervo/tutorial_tests.cpp
    cuervo/platform_tests.cpp
    cuervo/player_tests.cpp
    cuervo/profiler_tests.cpp
    cuervo/save_tests.cpp
    cuervo/locale_tests.cpp
    cuervo/shaping_tests.cpp
    cuervo/shaped_text_tests.cpp
    cuervo/ui_locale_tests.cpp
    cuervo/ime_tests.cpp
    cuervo/trace_tests.cpp
    cuervo/brand_tests.cpp
    cuervo/node_tests.cpp
    cuervo/hierarchy_tests.cpp
    cuervo/animation_tests.cpp
    cuervo/tilemap_tests.cpp
    cuervo/navigation_tests.cpp
    cuervo/license_tests.cpp
    cuervo/light_tests.cpp
    cuervo/lighting_script_tests.cpp
    cuervo/state_hash_tests.cpp
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
    list(APPEND CUERVO_TEST_SOURCES cuervo/ui_tests.cpp cuervo/ui_widget_tests.cpp)
endif()
if(CUERVO_FEATURE_PHYSICS)
    list(APPEND CUERVO_TEST_SOURCES
        cuervo/physics_tests.cpp
        cuervo/physics_joint_query_tests.cpp
        cuervo/physics_script_tests.cpp
        cuervo/physics_scenario_tests.cpp)
endif()

add_executable(cuervo_tests ${CUERVO_TEST_SOURCES})
cuervo_windows_app(cuervo_tests) # Windows: the same manifest as the shipped programs (cmake/Windows.cmake)
target_link_libraries(cuervo_tests PRIVATE cuervo cuervo_export cuervo_api_docs doctest SDL3::SDL3) # SDL: platform_tests.cpp
target_compile_definitions(cuervo_tests PRIVATE CUERVO_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
    CUERVO_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples" CUERVO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(CUERVO_TEXT_SHAPING AND CUERVO_TEST_FONT_ARABIC)
    target_compile_definitions(cuervo_tests PRIVATE CUERVO_TEST_FONT_ARABIC="${CUERVO_TEST_FONT_ARABIC}")
endif()
if(CUERVO_TEXT_SHAPING AND CUERVO_TEST_FONT_JAPANESE)
    target_compile_definitions(cuervo_tests PRIVATE CUERVO_TEST_FONT_JAPANESE="${CUERVO_TEST_FONT_JAPANESE}")
endif()
# The example plugins (examples/plugins, docs/PLUGINS.md) each test their language when it was built.
foreach(_plugin orbit noise hues)
    string(TOUPPER ${_plugin} _upper)
    if(TARGET ${_plugin}_plugin)
        add_dependencies(cuervo_tests ${_plugin}_plugin)
        if(TARGET ${_plugin}_plugin_static)
            add_dependencies(cuervo_tests ${_plugin}_plugin_static)
        endif()
        target_compile_definitions(cuervo_tests PRIVATE CUERVO_TEST_PLUGIN_${_upper}=1)
    else()
        target_compile_definitions(cuervo_tests PRIVATE CUERVO_TEST_PLUGIN_${_upper}=0)
    endif()
endforeach()
add_test(NAME cuervo_tests COMMAND cuervo_tests)
# MinGW-w64 and MSVC Debug take 5-7 minutes on the Windows laptop; slower machines need headroom.
set_tests_properties(cuervo_tests PROPERTIES TIMEOUT 1200)

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
    pluma/module_tests.cpp
    pluma/testing_tests.cpp
    pluma/ffi_tests.cpp
    pluma/reload_tests.cpp
    pluma/export_tests.cpp
    pluma/memory_tests.cpp
    pluma/aot_tests.cpp
    pluma/semantics_tests.cpp
    pluma/intmodel_tests.cpp
    pluma/stability_tests.cpp
    pluma/library_tests.cpp
    pluma/math_tests.cpp
    pluma/format_tests.cpp
    pluma/lsp_tests.cpp
    pluma/debug_tests.cpp
    pluma/dap_tests.cpp
    pluma/profiler_tests.cpp
    pluma/snapshot_tests.cpp
)
target_link_libraries(pluma_tests PRIVATE pluma doctest)
target_compile_definitions(pluma_tests PRIVATE CUERVO_SEMANTICS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pluma/semantics")
target_compile_definitions(pluma_tests PRIVATE PLUMA_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
    PLUMA_PRELUDE_DIR="${PROJECT_SOURCE_DIR}/cuervo/script/prelude")
add_test(NAME pluma_tests COMMAND pluma_tests)

# The embedding API from plain C (pluma/embed/pluma_embed.h).
add_executable(pluma_embed_test pluma/embed_test.c)
target_link_libraries(pluma_embed_test PRIVATE pluma)
set_target_properties(pluma_embed_test PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME pluma_embed COMMAND pluma_embed_test)

# Backend parity through the frame-capture path (tests/rhi_parity.cmake).
if(TARGET rhi_parity AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    add_test(NAME rhi_parity COMMAND ${CMAKE_COMMAND}
        -DRHI_PARITY=$<TARGET_FILE:rhi_parity>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DSHMUP=${PROJECT_SOURCE_DIR}/examples/shmup
        -DBENCH=${PROJECT_SOURCE_DIR}/examples/bench
        -DLIT=$<$<BOOL:${CUERVO_FEATURE_PHYSICS}>:${PROJECT_SOURCE_DIR}/examples/lantern_cave>
        -DWEBGPU=${CUERVO_WEBGPU}
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rhi_parity
        -P ${CMAKE_CURRENT_SOURCE_DIR}/rhi_parity.cmake)
    set_tests_properties(rhi_parity PROPERTIES TIMEOUT 300)
endif()

# The LSP and DAP programs' real stdout bytes (header framing; tests/stdio_framing.cmake).
if(TARGET cuervo_tools AND TARGET runtime)
    add_test(NAME stdio_framing COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/stdio_framing
        -P ${CMAKE_CURRENT_SOURCE_DIR}/stdio_framing.cmake)
    set_tests_properties(stdio_framing PROPERTIES TIMEOUT 120)
endif()

# docs/API.md is generated from the engine prelude (`cuervo-tools docs`), so
# it can't drift from what scripts can call. Only a build with every optional
# prelude compiled in generates the whole reference.
if(TARGET cuervo_tools AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES)
    add_test(NAME api_docs_up_to_date COMMAND cuervo_tools docs --check ${PROJECT_SOURCE_DIR}/docs/API.md)
    # ...and every public symbol in it has a doc comment (milestone 8.4).
    add_test(NAME api_docs_coverage COMMAND cuervo_tools docs --check-coverage)
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

# `cuervo --game --trace` writes a Chrome trace; ctest parses it (milestone 11.4): per-system zone spans with
# durations, draw-call and heap counters, thread metadata.
if(TARGET runtime AND CUERVO_PROFILING AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION AND NOT CMAKE_HOST_WIN32)
    add_test(NAME trace_shmup COMMAND ${CMAKE_COMMAND} -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup -DOUT=${CMAKE_CURRENT_BINARY_DIR}/trace_shmup.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/trace_shmup.cmake)
    set_tests_properties(trace_shmup PROPERTIES TIMEOUT 90)
endif()

# Every pinned dependency has its license text and notice (THIRD_PARTY_NOTICES.md).
add_test(NAME third_party_notices COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/third_party_notices.cmake)

# The migration report CLI on the shipped example (exit 0 = fits its tier).
add_test(NAME tier_check_shmup COMMAND cuervo_tools tier check ${PROJECT_SOURCE_DIR}/examples/shmup)
# ...and refuses claims the target platform does not provide (milestone 8.8, docs/support.json): the shmup
# fits every platform at its own tier, but a Full tier is desktop-only.
add_test(NAME tier_check_shmup_web COMMAND cuervo_tools tier check ${PROJECT_SOURCE_DIR}/examples/shmup --platform web)
add_test(NAME tier_check_shmup_full_linux COMMAND cuervo_tools tier check ${PROJECT_SOURCE_DIR}/examples/shmup --to full --platform linux)
add_test(NAME tier_check_shmup_full_web_refused COMMAND cuervo_tools tier check ${PROJECT_SOURCE_DIR}/examples/shmup --to full --platform web)
set_tests_properties(tier_check_shmup_full_web_refused PROPERTIES WILL_FAIL TRUE)
# Pluma unit tests (`@test`, LANGUAGE.md 9.27) of the package example and of a game that uses it.
add_test(NAME pluma_tests_tween COMMAND cuervo_tools test ${PROJECT_SOURCE_DIR}/examples/libs/tween)
add_test(NAME pluma_tests_garden COMMAND cuervo_tools test ${PROJECT_SOURCE_DIR}/examples/garden)

# The editor's core, headless (editor/, DESIGN.md 4.13).
if(TARGET editor_core)
    add_executable(editor_tests doctest_main.cpp editor/editor_tests.cpp editor/editor_6b_tests.cpp editor/editor_83_tests.cpp editor/editor_87_tests.cpp
        editor/editor_98_tests.cpp editor/editor_117c_tests.cpp editor/editor_117c_particles_tests.cpp editor/editor_117b_tests.cpp editor/editor_117b_viewports_tests.cpp editor/editor_net_tests.cpp editor/editor_controller_tests.cpp editor/scene_input_tests.cpp editor/editor_rml_tests.cpp editor/editor_tree_tests.cpp editor/editor_inspector_tests.cpp editor/editor_picker_tests.cpp editor/sprite_view_tests.cpp editor/timeline_tests.cpp)
    # Script editor (ui-rmlui-script): separate lines so they merge cleanly with other test additions.
    target_sources(editor_tests PRIVATE editor/text_buffer_tests.cpp editor/script_syntax_tests.cpp editor/script_workspace_tests.cpp editor/script_find_tests.cpp editor/script_assist_tests.cpp editor/script_debug_tests.cpp)
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
    # The default editor is the RmlUi one. The same script through the deprecated ImGui editor
    # (--imgui) while it is compiled in, and through the --rml-ui alias (still accepted).
    if(CUERVO_EDITOR_IMGUI)
        add_test(NAME editor_level_imgui COMMAND ${CMAKE_COMMAND}
            -DEDITOR=$<TARGET_FILE:editor> -DEDITOR_ARGS=--imgui
            -DRUNTIME=$<TARGET_FILE:runtime>
            -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_level_imgui
            -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_level.cmake)
        set_tests_properties(editor_level_imgui PROPERTIES TIMEOUT 120)
    endif()
    add_test(NAME editor_level_rml COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor> -DEDITOR_ARGS=--rml-ui
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_level_rml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_level.cmake)
    set_tests_properties(editor_level_rml PROPERTIES TIMEOUT 120)
endif()

# Undockable panels (11.7b): a panel undocked into a second SDL window is restored from the saved
# layout (tests/editor_viewports.cmake; skips itself where no second offscreen window can be made).
if(TARGET editor AND NOT WIN32 AND CUERVO_EDITOR_IMGUI)
    add_test(NAME editor_viewports COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_viewports
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_viewports.cmake)
    set_tests_properties(editor_viewports PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "SKIPPED:")
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

# Milestone 9.4's done-when: the animated character blends between states under
# scripted input, with the same state hash on 1 and 4 job threads, a replay that
# verifies on both, and a scene file that round-trips (tests/anim_character.cmake).
if(TARGET editor AND TARGET runtime AND TARGET cuervo_tools)
    add_test(NAME anim_character COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/anim_character
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/anim_character
        -DEXPECT=4f6b91c3dca0c7be
        -P ${CMAKE_CURRENT_SOURCE_DIR}/anim_character.cmake)
    set_tests_properties(anim_character PROPERTIES TIMEOUT 240)

    # Milestone 9.5's done-when: a top-down level is painted and an agent paths
    # around obstacles, with the same state hash on 1 and 4 job threads, a replay
    # that verifies on both, and a scene file that round-trips.
    add_test(NAME topdown_nav COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/topdown_nav
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/topdown_nav
        -DEXPECT=882ea24679ca2d41
        -P ${CMAKE_CURRENT_SOURCE_DIR}/topdown_nav.cmake)
    set_tests_properties(topdown_nav PROPERTIES TIMEOUT 240)

    # Milestone 9.6's done-when: a platformer character runs, jumps and pushes
    # bodies deterministically, with the same state hash on 1 and 4 job threads, a replay
    # that verifies on both, and a scene file that round-trips.
    if(CUERVO_FEATURE_PHYSICS)
        add_test(NAME platformer COMMAND ${CMAKE_COMMAND}
            -DEDITOR=$<TARGET_FILE:editor>
            -DRUNTIME=$<TARGET_FILE:runtime>
            -DTOOLS=$<TARGET_FILE:cuervo_tools>
            -DPROJECT=${PROJECT_SOURCE_DIR}/examples/platformer
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/platformer
            -DEXPECT=e458fa6c80754cc5
            -P ${CMAKE_CURRENT_SOURCE_DIR}/platformer.cmake)
        set_tests_properties(platformer PROPERTIES TIMEOUT 240)

        # Milestone 9.8's done-when: a physics-based platformer with 2D lighting
        # is playable (tests/lantern_cave.cmake).
        add_test(NAME lantern_cave COMMAND ${CMAKE_COMMAND}
            -DEDITOR=$<TARGET_FILE:editor>
            -DRUNTIME=$<TARGET_FILE:runtime>
            -DTOOLS=$<TARGET_FILE:cuervo_tools>
            -DPROJECT=${PROJECT_SOURCE_DIR}/examples/lantern_cave
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/lantern_cave
            -DEXPECT=2dd8beccd71bc67e
            -P ${CMAKE_CURRENT_SOURCE_DIR}/lantern_cave.cmake)
        set_tests_properties(lantern_cave PROPERTIES TIMEOUT 300)
    endif()
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
    if(CUERVO_EDITOR_IMGUI)
        add_test(NAME editor_replay_imgui COMMAND ${CMAKE_COMMAND}
            -DEDITOR=$<TARGET_FILE:editor> -DEDITOR_ARGS=--imgui
            -DRUNTIME=$<TARGET_FILE:runtime>
            -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_replay_imgui
            -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_replay.cmake)
        set_tests_properties(editor_replay_imgui PROPERTIES TIMEOUT 180)
    endif()
    add_test(NAME editor_replay_rml COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor> -DEDITOR_ARGS=--rml-ui
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_replay_rml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_replay.cmake)
    set_tests_properties(editor_replay_rml PROPERTIES TIMEOUT 180)
endif()

# Milestone 9.3's done-when: the settings menu and inventory of examples/ui_menus
# driven by keyboard, gamepad and pointer, recorded and replayed with the same
# state hashes (tests/ui_menus.cmake).
if(TARGET editor AND TARGET runtime)
    add_test(NAME ui_menus COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/ui_menus
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_menus
        -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_menus.cmake)
    set_tests_properties(ui_menus PROPERTIES TIMEOUT 240)
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
    # Two AOT builds: about a minute each with MinGW-w64 GCC, which compiles the generated C slowest.
    set_tests_properties(export_shmup PROPERTIES TIMEOUT 360)
    # The tutorial game (docs/GUIDE.md, examples/tutorial) takes the same path: it must pack, export (source
    # and AOT), ship alone and reach the dev run's hash (milestone 8.4: the tutorial builds in CI).
    add_test(NAME export_tutorial COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/tutorial
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/export_tutorial
        -DNAME=tutorial
        -DLAUNCHER=starfall
        -DEXPECT=519ec4a631d8254d
        -P ${CMAKE_CURRENT_SOURCE_DIR}/export_shmup.cmake)
    set_tests_properties(export_tutorial PROPERTIES TIMEOUT 360)
    # ...and `cuervo --game examples/tutorial` runs headless on GL with the same hash.
    add_test(NAME run_tutorial COMMAND $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/tutorial
        --backend gl --no-audio --screenshot ${CMAKE_CURRENT_BINARY_DIR}/run_tutorial.ppm)
    set_tests_properties(run_tutorial PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "state hash 519ec4a631d8254d")
    # Windows has no EGL for SDL's offscreen driver: it opens a real window.
    if(NOT CMAKE_HOST_WIN32)
        set_tests_properties(run_tutorial PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    endif()
endif()

# Milestone 11.6: docs/CLI.md is generated from the flag tables (`cuervo-tools cli-docs`), like docs/API.md.
if(TARGET cuervo_tools)
    add_test(NAME cli_reference_up_to_date COMMAND cuervo_tools cli-docs --check ${PROJECT_SOURCE_DIR}/docs/CLI.md)
endif()

# ...and the automation surface of `cuervo --game` (tests/cli_automation.cmake): --press axes move a player
# (state hash), a script that doesn't compile exits 2, a panic exits 3, the screenshot tick default.
if(TARGET runtime AND NOT EMSCRIPTEN AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    add_test(NAME cli_automation COMMAND ${CMAKE_COMMAND}
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/shmup
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_automation
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_automation.cmake)
    set_tests_properties(cli_automation PROPERTIES TIMEOUT 240)
endif()

option(CUERVO_SLOW_TESTS "Add tests that build engines (stripped-player exports)" OFF)

# Milestone 8.7's stabilization sweep: every example through the whole release path
# (run, scene round trip, replay, hot reload, bytecode + AOT exports), one test per
# example (tests/examples_sweep.cmake). Needs every feature the examples use.
if(TARGET editor AND TARGET runtime AND TARGET cuervo_tools AND NOT EMSCRIPTEN AND CUERVO_FEATURE_AUDIO AND CUERVO_FEATURE_UI AND CUERVO_FEATURE_PARTICLES AND CUERVO_FEATURE_COLLISION)
    # name|--press
    set(_anim_press "")
    foreach(_t RANGE 20 90)
        string(APPEND _anim_press "${_t}:right,")
    endforeach()
    string(APPEND _anim_press "60:jump,150:jump,200:jump")
    set(_sweeps
        "anim_character|${_anim_press}"
        "bench|20:more,60:more,100:more"
        "editor_level|30:shoot,31:shoot,90:shoot,120:shoot"
        "garden|20:more,40:more,60:more"
        "node_level|"
        "shmup|30:ui_accept,90:shoot,91:shoot,92:shoot,93:shoot,150:bomb"
        "tictak|10:ui_down,20:ui_accept,40:ui_right,50:ui_accept,70:ui_down,80:ui_accept,100:pause,110:ui_accept"
        "tutorial|10:ui_accept,40:ui_accept,80:ui_right,120:ui_accept"
        "ui_menus|5:ui_down,10:ui_down,15:ui_right,20:ui_accept,30:ui_tab_next,33:ui_down,36:ui_accept")
    if(CUERVO_FEATURE_PHYSICS)
        list(APPEND _sweeps "lantern_cave|10-114:move_right,60:jump,124:jump,138-240:move_right")
    endif()
    # CUERVO_SLOW_TESTS adds the stripped player to the exports (it builds a Release engine the first time).
    set(_sweep_player_cache "")
    set(_sweep_timeout 600)
    if(CUERVO_SLOW_TESTS)
        set(_sweep_player_cache ${CMAKE_CURRENT_BINARY_DIR}/player_cache)
        set(_sweep_timeout 1800)
    endif()
    foreach(_sweep ${_sweeps})
        string(REGEX MATCH "^([^|]*)\\|(.*)$" _m "${_sweep}")
        set(_name "${CMAKE_MATCH_1}")
        # Kept now: the `if(... MATCHES ...)` below resets CMAKE_MATCH_*.
        set(_press "${CMAKE_MATCH_2}")
        # A hot reload that restarts no coroutine and allocates no emitter keeps the run bit for bit.
        set(_exact OFF)
        if(_name MATCHES "^(bench|tictak|ui_menus)$")
            set(_exact ON)
        endif()
        add_test(NAME examples_sweep_${_name} COMMAND ${CMAKE_COMMAND}
            -DTOOLS=$<TARGET_FILE:cuervo_tools>
            -DRUNTIME=$<TARGET_FILE:runtime>
            -DEDITOR=$<TARGET_FILE:editor>
            -DPROJECT=${PROJECT_SOURCE_DIR}/examples/${_name}
            -DNAME=${_name}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/examples_sweep_${_name}
            "-DPRESS=${_press}"
            -DRELOAD_EXACT=${_exact}
            -DPLAYER_CACHE=${_sweep_player_cache}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/examples_sweep.cmake)
        set_tests_properties(examples_sweep_${_name} PROPERTIES TIMEOUT ${_sweep_timeout})
    endforeach()
endif()

# Milestone 8.9: the fresh-checkout user path end to end (new project, edit and save a scene, play it, add a
# script, hot reload, export, launch the shipped game elsewhere; tests/workflow_smoke.cmake).
if(TARGET editor AND TARGET runtime AND TARGET cuervo_tools AND NOT EMSCRIPTEN)
    set(_workflow_player_cache "")
    set(_workflow_timeout 600)
    if(CUERVO_SLOW_TESTS)
        set(_workflow_player_cache ${CMAKE_CURRENT_BINARY_DIR}/player_cache)
        set(_workflow_timeout 1800)
    endif()
    add_test(NAME workflow_smoke COMMAND ${CMAKE_COMMAND}
        -DTOOLS=$<TARGET_FILE:cuervo_tools>
        -DRUNTIME=$<TARGET_FILE:runtime>
        -DEDITOR=$<TARGET_FILE:editor>
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/workflow_smoke
        -DPLAYER_CACHE=${_workflow_player_cache}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/workflow_smoke.cmake)
    set_tests_properties(workflow_smoke PROPERTIES TIMEOUT ${_workflow_timeout})
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

# Networked play end to end (milestone 11.1 slice 6): two headless `cuervo` processes play examples/netduel
# over UDP loopback and through tools/net/relay.py, lockstep and rollback, and must print the same state and
# snapshot hash; the session each one recorded is verified offline by the editor when it is built.
# Ports are picked free by the script; skipped on Windows (the offscreen video driver is not there).
if(TARGET runtime AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN AND NOT CMAKE_HOST_WIN32)
    set(netplay_editor "")
    if(TARGET editor)
        set(netplay_editor --editor $<TARGET_FILE:editor>)
    endif()
    foreach(mode lockstep rollback)
        add_test(NAME netplay_udp_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netduel
                    --mode ${mode} ${netplay_editor})
        add_test(NAME netplay_relay_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netduel
                    --mode ${mode} --relay ${PROJECT_SOURCE_DIR}/tools/net/relay.py)
        # A game that starts its session at tick 50 (the replay keeps the ticks before it).
        add_test(NAME netplay_late_udp_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netduel
                    --mode ${mode} --late 50 ${netplay_editor})
        set_tests_properties(netplay_udp_${mode} netplay_relay_${mode} netplay_late_udp_${mode}
                             PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
        # Three players in examples/netarena: a UDP mesh and a relay room; the same final hashes everywhere.
        add_test(NAME netplay_mesh3_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netarena
                    --players 3 --mode ${mode} ${netplay_editor})
        add_test(NAME netplay_relay3_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netarena
                    --players 3 --mode ${mode} --relay ${PROJECT_SOURCE_DIR}/tools/net/relay.py ${netplay_editor})
        set_tests_properties(netplay_mesh3_${mode} netplay_relay3_${mode} PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
    endforeach()
    # A three-player match whose session starts at tick 40 (mesh, lockstep), and four players on a relay (rollback).
    add_test(NAME netplay_late_mesh3_lockstep
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netarena
                --players 3 --late 40 ${netplay_editor})
    add_test(NAME netplay_relay4_rollback
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/netarena
                --players 4 --mode rollback --relay ${PROJECT_SOURCE_DIR}/tools/net/relay.py ${netplay_editor})
    set_tests_properties(netplay_late_mesh3_lockstep netplay_relay4_rollback PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
endif()

# assets/brand/ only changes through tools/sync_brand.py (DESIGN.md 4.16): its
# files match the hashes the script recorded in SOURCE.txt. Needs no design kit.
if(Python3_Interpreter_FOUND)
    add_test(NAME brand_assets
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/sync_brand.py --verify)
    # The Rust and C# raw layers match cuervo/plugin/cuervo_plugin.h (they are
    # generated from it, DESIGN.md 4.18).
    # docs/ROADMAP.md is generated from the milestones table (DESIGN.md 6).
    add_test(NAME roadmap_up_to_date
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/gen_roadmap.py --check)
    add_test(NAME plugin_abi_bindings
        COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/gen_plugin_abi.py --check
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

# Per-export stripped players end to end (milestone 7.5, tests/export_stripped.cmake):
# export examples/tictak with --engine stripped, ship only dist/, and compare
# it with the dev run. Off by default because the first run configures and
# builds a Release engine; the player cache stays in the build tree.
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

# The documentation site (milestone 8.4, tools/build_site.py, DESIGN.md 4.16): `cmake --build <dir> --target
# docs_site` writes <dir>/site. The ctest builds it and fails on any Markdown link that points nowhere or any
# broken internal link/fragment in the output. The brand fonts ship with it, so it needs the editor's fonts.
find_program(CUERVO_PYTHON3 python3)
if(CUERVO_PYTHON3)
    set(_site_script ${PROJECT_SOURCE_DIR}/tools/build_site.py)
    # The support matrix (milestone 8.8): docs/STATUS.md and README.md are generated from
    # docs/support.json; this fails when they are stale or the source is inconsistent.
    add_test(NAME support_matrix_up_to_date COMMAND ${CUERVO_PYTHON3} -I ${PROJECT_SOURCE_DIR}/tools/support_matrix.py --check)
    add_test(NAME support_matrix_unit COMMAND ${CUERVO_PYTHON3} -I ${PROJECT_SOURCE_DIR}/tools/test_support_matrix.py)
    # Performance baselines (milestone 8.6, tools/bench.py, docs/PERFORMANCE.md).
    add_test(NAME bench_compare_unit COMMAND ${CUERVO_PYTHON3} -I ${PROJECT_SOURCE_DIR}/tools/test_bench.py)
    set_tests_properties(bench_compare_unit PROPERTIES LABELS "perf")
    if(TARGET runtime AND NOT WIN32 AND NOT EMSCRIPTEN)
        add_test(NAME bench_smoke COMMAND ${CMAKE_COMMAND} -DPYTHON=${CUERVO_PYTHON3}
            -DBENCH=${PROJECT_SOURCE_DIR}/tools/bench.py -DBUILD=${CMAKE_BINARY_DIR}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/bench_smoke -P ${CMAKE_CURRENT_SOURCE_DIR}/bench_smoke.cmake)
        set_tests_properties(bench_smoke PROPERTIES LABELS "perf" TIMEOUT 300)
    endif()
    add_test(NAME docs_site_unit COMMAND ${CUERVO_PYTHON3} -I ${PROJECT_SOURCE_DIR}/tools/test_build_site.py
        $<$<TARGET_EXISTS:plm>:--plm> $<$<TARGET_EXISTS:plm>:$<TARGET_FILE:plm>>)
    if(DEFINED CUERVO_FONT_DISPLAY AND DEFINED CUERVO_FONT_UI AND DEFINED CUERVO_FONT_UI_BOLD AND DEFINED CUERVO_FONT_MONO)
        set(_site_cmd ${CUERVO_PYTHON3} -I ${_site_script}
            --font display=${CUERVO_FONT_DISPLAY} --font ui=${CUERVO_FONT_UI}
            --font ui-bold=${CUERVO_FONT_UI_BOLD} --font mono=${CUERVO_FONT_MONO})
        add_custom_target(docs_site COMMAND ${_site_cmd} -o ${CMAKE_BINARY_DIR}/site
            COMMENT "Building the documentation site" VERBATIM)
        add_test(NAME docs_site COMMAND ${_site_cmd} -o ${CMAKE_CURRENT_BINARY_DIR}/site)
        set_tests_properties(docs_site PROPERTIES TIMEOUT 120)
    endif()
endif()

# The editor's style against ImGui's own defaults (theme.cpp only; no window).
if(TARGET editor AND TARGET imgui AND CUERVO_EDITOR_IMGUI)
    add_executable(editor_ui_tests doctest_main.cpp editor/editor_ui_tests.cpp ${PROJECT_SOURCE_DIR}/editor/imgui_theme.cpp ${PROJECT_SOURCE_DIR}/editor/theme.cpp)
    target_link_libraries(editor_ui_tests PRIVATE cuervo imgui doctest SDL3::SDL3)
    target_compile_definitions(editor_ui_tests PRIVATE
        CUERVO_FONT_UI="" CUERVO_FONT_MONO="" CUERVO_FONT_DISPLAY="")
    add_test(NAME editor_ui_tests COMMAND editor_ui_tests)
endif()

# The RmlUi editor's start-up switches (tests/editor_rml_cli.cmake), offscreen GL.
if(TARGET editor AND NOT WIN32)
    add_test(NAME editor_rml_cli COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_rml_cli
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_rml_cli.cmake)
    set_tests_properties(editor_rml_cli PROPERTIES TIMEOUT 120)
endif()

# Golden screenshots of the RmlUi editor (tests/editor_golden.cmake, tests/golden/README.md), offscreen GL.
if(TARGET editor AND TARGET cuervo AND NOT WIN32)
    add_executable(golden_diff editor/golden_diff.cpp)
    target_link_libraries(golden_diff PRIVATE cuervo)
    add_test(NAME editor_golden COMMAND ${CMAKE_COMMAND}
        -DEDITOR=$<TARGET_FILE:editor> -DDIFF=$<TARGET_FILE:golden_diff>
        -DPROJECT=${PROJECT_SOURCE_DIR}/examples/editor_level
        -DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/editor_golden
        -P ${CMAKE_CURRENT_SOURCE_DIR}/editor_golden.cmake)
    set_tests_properties(editor_golden PROPERTIES TIMEOUT 180)
endif()

# The Phase 11 capstone (docs/STATUS.md, examples/showcase_duel): a localized, post-processed, atlas-based
# two-player duel. `showcase_make_efont` regenerates its shaped fallback fonts (see the example's README); it is
# not built by default. The tests: two headless processes play it over UDP (lockstep and rollback) and agree on
# the state hash, and a run per language renders and replays.
if(TARGET runtime AND NOT EMSCRIPTEN)
    add_executable(showcase_make_efont EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/examples/showcase_duel/tools/make_efont.cpp)
    target_link_libraries(showcase_make_efont PRIVATE cuervo)
endif()
if(TARGET runtime AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN AND NOT CMAKE_HOST_WIN32)
    set(showcase_editor "")
    if(TARGET editor)
        set(showcase_editor --editor $<TARGET_FILE:editor>)
    endif()
    foreach(mode lockstep rollback)
        add_test(NAME showcase_duel_netplay_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/showcase_duel
                    --mode ${mode} ${showcase_editor})
        add_test(NAME showcase_duel_netplay_relay_${mode}
            COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/net/netplay_test.py
                    --cuervo $<TARGET_FILE:runtime> --game ${PROJECT_SOURCE_DIR}/examples/showcase_duel
                    --mode ${mode} --relay ${PROJECT_SOURCE_DIR}/tools/net/relay.py)
        set_tests_properties(showcase_duel_netplay_${mode} showcase_duel_netplay_relay_${mode}
                             PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endforeach()
    set(showcase_editor_def "")
    if(TARGET editor)
        set(showcase_editor_def -DEDITOR=$<TARGET_FILE:editor>)
    endif()
    add_test(NAME showcase_duel_languages
        COMMAND ${CMAKE_COMMAND} -DCUERVO=$<TARGET_FILE:runtime> ${showcase_editor_def}
                -DGAME=${PROJECT_SOURCE_DIR}/examples/showcase_duel
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/showcase_duel
                -P ${CMAKE_CURRENT_SOURCE_DIR}/showcase_duel.cmake)
    set_tests_properties(showcase_duel_languages PROPERTIES TIMEOUT 180)
endif()
