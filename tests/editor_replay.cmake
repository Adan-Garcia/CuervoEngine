# Play-in-editor and replays through the shipped binaries (DESIGN.md 4.13,
# "Clarified (Phase 6B)"):
#   1. `cuervo --game` plays the shmup headless with scripted input to tick
#      240 and records the run (--record); its log has the state hash
#   2. `cuervo-editor --replay-verify` replays it on 1 and 8 job threads:
#      every tick's state hash must match (exit 0)
#   3. the same replay against a copy of the project whose scripts drifted
#      must fail (exit 1, "mismatch at tick 1")
#   4. `cuervo-editor --script-commands` plays the project in the editor to
#      tick 240 with the same input and must reach the runtime's hash
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DPROJECT=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

set(presses "30:ui_accept,90:shoot,91:shoot,92:shoot,93:shoot,150:bomb")
execute_process(
    COMMAND "${RUNTIME}" --game "${PROJECT}" --backend gl --no-audio --press "${presses}"
            --record "${OUT}/run.replay" --screenshot "${OUT}/run.ppm"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REGEX MATCH "state hash ([0-9a-f]+)" m "${out}${err}")
set(runtime_hash "${CMAKE_MATCH_1}")
if(NOT code EQUAL 0 OR runtime_hash STREQUAL "" OR NOT EXISTS "${OUT}/run.replay")
    message(FATAL_ERROR "cuervo --game --record failed (${code}):\n${out}\n${err}")
endif()

foreach(jobs 1 8)
    execute_process(
        COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/run.replay" --jobs ${jobs}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "240 ticks replayed on ${jobs} thread")
        message(FATAL_ERROR "--replay-verify on ${jobs} job(s) failed (${code}):\n${out}\n${err}")
    endif()
    string(REGEX MATCH "final state hash ([0-9a-f]+)" m "${out}${err}")
    if(NOT CMAKE_MATCH_1 STREQUAL runtime_hash)
        message(FATAL_ERROR "--replay-verify ended at ${CMAKE_MATCH_1}, the runtime at ${runtime_hash}")
    endif()
endforeach()

# Scripts that drifted since the recording: a new static changes the state
# hash from the first tick.
file(COPY "${PROJECT}/" DESTINATION "${OUT}/drifted")
file(APPEND "${OUT}/drifted/scripts/game.plm" "\nclass Drift {\n    static var amount = 1\n}\n")
execute_process(
    COMMAND "${EDITOR}" --project "${OUT}/drifted" --replay-verify "${OUT}/run.replay" --jobs 1
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "mismatch at tick 1 of 240")
    message(FATAL_ERROR "--replay-verify accepted a drifted project (${code}):\n${out}\n${err}")
endif()

# Play-in-editor reaches the runtime's hash with the same input.
file(WRITE "${OUT}/play.cmds"
    "play\n"
    "step 29\nhold ui_accept\nstep 1\nrelease ui_accept\n"
    "step 59\nhold shoot\nstep 4\nrelease shoot\n"
    "step 56\nhold bomb\nstep 1\nrelease bomb\n"
    "step 90\n"
    "expect-tick 240\n"
    "expect-hash ${runtime_hash}\n"
    "stop\n")
execute_process(
    COMMAND "${EDITOR}" --project "${PROJECT}" --script-commands "${OUT}/play.cmds"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "play-in-editor did not reach the runtime's hash ${runtime_hash} (${code}):\n${out}\n${err}")
endif()
message(STATUS "runtime and play-in-editor agree at tick 240: ${runtime_hash}; the replay verifies on 1 and 8 threads")
