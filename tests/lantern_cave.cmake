# Milestone 9.8's "done when" (DESIGN.md 6, 4.17): a physics-based platformer
# with 2D lighting is playable. End to end through the shipped binaries on
# examples/lantern_cave (tilemap collision and occluders, a CharacterBody2D
# with an AnimationPlayer walk cycle, PointLight2D / AmbientLight2D lighting):
#   1. cuervo --game plays it headless (GL, offscreen) to tick 240 with scripted
#      --press input on --jobs 1 and --jobs 4: the same state hash (and EXPECT,
#      a pinned regression value)
#   2. the recorded input replays in cuervo-editor --replay-verify on 1 and 4
#      job threads, every tick's hash matching
#   3. a scripted playthrough collects every gem and leaves through the door
#      ("Escaped the cave" in the log) before tick 600
#   4. the cave scene round-trips through the text view, and the committed
#      authoring text builds the committed scene
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DTOOLS=... -DPROJECT=... -DOUT=... -DEXPECT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

# The route through the cave: over the pillar, onto the ledge, into the pit,
# through the tunnel and up the steps to the door.
set(route "10-114:move_right,60:jump,124:jump,138-900:move_right,262:jump,446:jump,466:jump,486:jump,506:jump,526:jump")

function(run_game jobs ticks result log)
    set(ENV{CUERVO_SCREENSHOT_TICK} ${ticks})
    execute_process(
        COMMAND "${RUNTIME}" --game "${PROJECT}" --backend gl --no-audio --press "${route}" --jobs ${jobs}
                --record "${OUT}/run_${jobs}_${ticks}.replay" --screenshot "${OUT}/run_${jobs}_${ticks}.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    unset(ENV{CUERVO_SCREENSHOT_TICK})
    string(REGEX MATCH "at tick ${ticks} \\(state hash ([0-9a-f]+)\\)" m "${out}${err}")
    set(found "${CMAKE_MATCH_1}")
    if(NOT code EQUAL 0 OR found STREQUAL "")
        message(FATAL_ERROR "cuervo --game --jobs ${jobs} failed (${code}):\n${out}\n${err}")
    endif()
    if(err MATCHES "\\[error\\]|panic|\\[warn")
        message(FATAL_ERROR "problems while running with --jobs ${jobs}:\n${err}")
    endif()
    set(${result} "${found}" PARENT_SCOPE)
    set(${log} "${out}${err}" PARENT_SCOPE)
endfunction()

run_game(1 240 hash1 log1)
run_game(4 240 hash4 log4)
if(NOT hash1 STREQUAL hash4)
    message(FATAL_ERROR "state hash at tick 240 differs between --jobs 1 (${hash1}) and --jobs 4 (${hash4})")
endif()
if(DEFINED EXPECT AND NOT EXPECT STREQUAL "" AND NOT hash1 STREQUAL EXPECT)
    message(FATAL_ERROR "state hash at tick 240 is ${hash1}, expected ${EXPECT}")
endif()

foreach(jobs 1 4)
    execute_process(
        COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/run_1_240.replay" --jobs ${jobs}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "240 ticks replayed on ${jobs} thread")
        message(FATAL_ERROR "--replay-verify on ${jobs} job(s) failed (${code}):\n${out}\n${err}")
    endif()
    string(REGEX MATCH "final state hash ([0-9a-f]+)" m "${out}${err}")
    if(NOT CMAKE_MATCH_1 STREQUAL hash1)
        message(FATAL_ERROR "--replay-verify ended at ${CMAKE_MATCH_1}, the runtime at ${hash1}")
    endif()
endforeach()

# The whole level, played.
run_game(1 600 hash_end log_end)
if(NOT log_end MATCHES "Escaped the cave at tick ([0-9]+) with 4 gems")
    message(FATAL_ERROR "the scripted playthrough did not escape with every gem:\n${log_end}")
endif()
set(escaped_at "${CMAKE_MATCH_1}")

# Scene round trip through the text view.
set(scene "${PROJECT}/scenes/cave.scene")
execute_process(COMMAND "${TOOLS}" scene dump "${scene}" OUTPUT_FILE "${OUT}/cave.dump.txt" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools scene dump failed (${code})")
endif()
execute_process(COMMAND "${TOOLS}" scene build "${OUT}/cave.dump.txt" "${OUT}/cave.rebuilt.scene" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools scene build (of the dump) failed (${code})")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/cave.rebuilt.scene" "${scene}" RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "dump -> build did not reproduce cave.scene")
endif()
execute_process(COMMAND "${TOOLS}" scene build "${PROJECT}/authoring/cave.txt" "${OUT}/cave.authored.scene" RESULT_VARIABLE code)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/cave.authored.scene" "${scene}" RESULT_VARIABLE differs)
if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
    message(FATAL_ERROR "authoring/cave.txt does not build to the committed cave.scene (run make_scene.py)")
endif()

message(STATUS "lantern_cave: state hash ${hash1} at tick 240 on 1 and 4 jobs; the replay verifies on 1 and 4 threads; "
               "the playthrough escapes at tick ${escaped_at}; the scene round-trips")
