# Milestone 8.2's "done when" (DESIGN.md 6, 4.16): a level using every new
# node is built in the editor without code and round-trips through scene files
# with the same state hash. End to end through the shipped binaries:
#   1. copy examples/node_level (project.toml, assets, level.cmds; no scene,
#      no scripts) to OUT
#   2. cuervo-editor --script-commands level.cmds builds and saves the level;
#      it must equal the checked-in scenes/level.scene byte for byte
#   3. cuervo --game OUT runs it headless (GL, offscreen) to tick 240 and
#      screenshots it: state hash H
#   4. the scene goes through the text view and back (cuervo-tools scene
#      dump | build): same bytes, and cuervo --game on the rebuilt file
#      reaches H again
#   5. the editor opens the saved scene, plays it to tick 240 and must
#      reach H (the editor-built scene loaded back from its file)
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DTOOLS=... -DPROJECT=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(COPY "${PROJECT}/project.toml" "${PROJECT}/level.cmds" "${PROJECT}/assets" DESTINATION "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

execute_process(
    COMMAND "${EDITOR}" --project "${OUT}" --script-commands "${OUT}/level.cmds"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT EXISTS "${OUT}/scenes/level.scene")
    message(FATAL_ERROR "cuervo-editor --script-commands failed (${code}):\n${out}\n${err}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/scenes/level.scene" "${PROJECT}/scenes/level.scene"
                RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "the editor-built level differs from examples/node_level/scenes/level.scene")
endif()

function(run_game label result)
    execute_process(
        COMMAND "${RUNTIME}" --game "${OUT}" --backend gl --no-audio --screenshot "${OUT}/${label}.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX MATCH "at tick 240 \\(state hash ([0-9a-f]+)\\)" m "${out}${err}")
    set(found "${CMAKE_MATCH_1}")
    if(NOT code EQUAL 0 OR found STREQUAL "")
        message(FATAL_ERROR "cuervo --game failed on the ${label} level (${code}):\n${out}\n${err}")
    endif()
    if(err MATCHES "\\[error\\]|panic")
        message(FATAL_ERROR "errors while running the ${label} level:\n${err}")
    endif()
    set(${result} "${found}" PARENT_SCOPE)
endfunction()

run_game(editor_built hash)

# Text view round trip: dump, build, same bytes, same run.
execute_process(COMMAND "${TOOLS}" scene dump "${OUT}/scenes/level.scene" OUTPUT_FILE "${OUT}/level.txt"
                RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools scene dump failed (${code})")
endif()
file(RENAME "${OUT}/scenes/level.scene" "${OUT}/editor_level.scene")
execute_process(COMMAND "${TOOLS}" scene build "${OUT}/level.txt" "${OUT}/scenes/level.scene" RESULT_VARIABLE code)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/scenes/level.scene" "${OUT}/editor_level.scene"
                RESULT_VARIABLE differs)
if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
    message(FATAL_ERROR "dump + build of the level did not give back the same scene file (${code})")
endif()
run_game(rebuilt rebuilt_hash)
if(NOT rebuilt_hash STREQUAL hash)
    message(FATAL_ERROR "the rebuilt level reached ${rebuilt_hash}, the editor-built one ${hash}")
endif()

# The editor plays the scene it loads back from the file to the same hash.
file(WRITE "${OUT}/play.cmds"
    "open scenes/level.scene\n"
    "play scene\n"
    "step 240\n"
    "expect-tick 240\n"
    "expect-hash ${hash}\n"
    "stop\n"
    "expect-clean\n")
execute_process(
    COMMAND "${EDITOR}" --project "${OUT}" --no-audio --script-commands "${OUT}/play.cmds"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "playing the loaded level in the editor did not reach ${hash} (${code}):\n${out}\n${err}")
endif()
message(STATUS "node_level: editor-built, rebuilt and editor-played runs agree at tick 240: ${hash}")
