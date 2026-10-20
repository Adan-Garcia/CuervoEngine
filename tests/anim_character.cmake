# Milestone 9.4's "done when" (DESIGN.md 6, 4.17): an animated character whose
# AnimationTree blends between states under input, with the replay hash
# unchanged at 1 and N threads. End to end through the shipped binaries on
# examples/anim_character:
#   1. cuervo --game plays it headless (GL, offscreen) to tick 240 with scripted
#      --press input (walk right, jump, walk left, jump) on --jobs 1 and --jobs 4:
#      the state hash must be the same (and equal EXPECT, a pinned regression value)
#   2. the same input is recorded (--record); cuervo-editor --replay-verify replays
#      it on 1 and 4 job threads and every tick's hash must match
#   3. scene round trip: the character scene (hierarchy, animations, state machine)
#      goes through the text view and back to the same bytes, and the committed
#      authoring text builds the committed scene
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DTOOLS=... -DPROJECT=... -DOUT=... -DEXPECT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

set(presses "")
foreach(t RANGE 20 90)
    string(APPEND presses "${t}:right,")
endforeach()
string(APPEND presses "60:jump,")
foreach(t RANGE 130 160)
    string(APPEND presses "${t}:left,")
endforeach()
string(APPEND presses "150:jump,200:jump")

function(run_game jobs result)
    execute_process(
        COMMAND "${RUNTIME}" --game "${PROJECT}" --backend gl --no-audio --press "${presses}" --jobs ${jobs}
                --record "${OUT}/run_${jobs}.replay" --screenshot "${OUT}/run_${jobs}.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX MATCH "at tick 240 \\(state hash ([0-9a-f]+)\\)" m "${out}${err}")
    set(found "${CMAKE_MATCH_1}")
    if(NOT code EQUAL 0 OR found STREQUAL "")
        message(FATAL_ERROR "cuervo --game --jobs ${jobs} failed (${code}):\n${out}\n${err}")
    endif()
    if(err MATCHES "\\[error\\]|panic|\\[warn")
        message(FATAL_ERROR "problems while running with --jobs ${jobs}:\n${err}")
    endif()
    set(${result} "${found}" PARENT_SCOPE)
endfunction()

run_game(1 hash1)
run_game(4 hash4)
if(NOT hash1 STREQUAL hash4)
    message(FATAL_ERROR "state hash at tick 240 differs between --jobs 1 (${hash1}) and --jobs 4 (${hash4})")
endif()
if(DEFINED EXPECT AND NOT EXPECT STREQUAL "" AND NOT hash1 STREQUAL EXPECT)
    message(FATAL_ERROR "state hash at tick 240 is ${hash1}, expected ${EXPECT}")
endif()

foreach(jobs 1 4)
    execute_process(
        COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/run_1.replay" --jobs ${jobs}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "240 ticks replayed on ${jobs} thread")
        message(FATAL_ERROR "--replay-verify on ${jobs} job(s) failed (${code}):\n${out}\n${err}")
    endif()
    string(REGEX MATCH "final state hash ([0-9a-f]+)" m "${out}${err}")
    if(NOT CMAKE_MATCH_1 STREQUAL hash1)
        message(FATAL_ERROR "--replay-verify ended at ${CMAKE_MATCH_1}, the runtime at ${hash1}")
    endif()
endforeach()

# Scene round trip through the text view.
set(scene "${PROJECT}/scenes/character.scene")
execute_process(COMMAND "${TOOLS}" scene dump "${scene}" OUTPUT_FILE "${OUT}/character.dump.txt" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools scene dump failed (${code})")
endif()
execute_process(COMMAND "${TOOLS}" scene build "${OUT}/character.dump.txt" "${OUT}/character.rebuilt.scene" RESULT_VARIABLE code)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools scene build (of the dump) failed (${code})")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/character.rebuilt.scene" "${scene}" RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "dump -> build did not reproduce character.scene")
endif()
execute_process(COMMAND "${TOOLS}" scene build "${PROJECT}/authoring/character.txt" "${OUT}/character.authored.scene" RESULT_VARIABLE code)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/character.authored.scene" "${scene}" RESULT_VARIABLE differs)
if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
    message(FATAL_ERROR "authoring/character.txt does not build to the committed character.scene (run make_scene.py and scene build)")
endif()

message(STATUS "anim_character: state hash ${hash1} at tick 240 on 1 and 4 jobs; the replay verifies on 1 and 4 threads; the scene round-trips")
