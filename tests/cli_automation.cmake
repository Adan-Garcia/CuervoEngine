# Milestone 11.6: the automation surface of `cuervo --game`, headless on GL.
#   1. --press with axis syntax ("40-150:move=1,0") moves the player through the
#      input frames: the state hash differs from a run without it, is the same on
#      every run and on 1 and 4 job threads, and an unknown axis changes nothing
#   2. a project whose scripts do not compile exits 2
#   3. a project whose script panics exits 3
#   4. a screenshot defaults to tick 240 and --screenshot-tick changes it
# Invoked by ctest with -DRUNTIME=... -DPROJECT=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

# Runs the game; sets <prefix>_CODE and <prefix>_LOG.
function(run prefix)
    execute_process(COMMAND "${RUNTIME}" --backend gl --no-audio ${ARGN}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${prefix}_CODE "${code}" PARENT_SCOPE)
    set(${prefix}_LOG "${out}${err}" PARENT_SCOPE)
endfunction()

function(hash_at tick log result)
    string(REGEX MATCH "at tick ${tick} \\(state hash ([0-9a-f]+)\\)" m "${log}")
    if(CMAKE_MATCH_1 STREQUAL "")
        message(FATAL_ERROR "no state hash at tick ${tick} in:\n${log}")
    endif()
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# 1. axis presses. The shmup's title waits for ui_accept; the ship fires so the
# bullets (hashed particles) record where it was.
set(start "30:ui_accept,40-150:shoot")
run(base --game "${PROJECT}" --screenshot "${OUT}/base.ppm" --screenshot-tick 150 --press "${start}")
run(axis --game "${PROJECT}" --screenshot "${OUT}/axis.ppm" --screenshot-tick 150 --press "${start},40-150:move=1,0")
run(axis1 --game "${PROJECT}" --screenshot "${OUT}/axis1.ppm" --screenshot-tick 150 --jobs 1 --press "${start},40-150:move=1,0")
run(axis4 --game "${PROJECT}" --screenshot "${OUT}/axis4.ppm" --screenshot-tick 150 --jobs 4 --press "${start},40-150:move=1,0")
run(mixed --game "${PROJECT}" --screenshot "${OUT}/mixed.ppm" --screenshot-tick 150 --press "${start},40-90:move=1,0,91-150:move=-1,0")
run(unknown --game "${PROJECT}" --screenshot "${OUT}/unknown.ppm" --screenshot-tick 150 --press "${start},40-150:nomove=1,0")
foreach(p base axis axis1 axis4 mixed unknown)
    if(NOT ${p}_CODE EQUAL 0)
        message(FATAL_ERROR "run '${p}' failed (${${p}_CODE}):\n${${p}_LOG}")
    endif()
    hash_at(150 "${${p}_LOG}" ${p}_HASH)
endforeach()
if(axis_HASH STREQUAL base_HASH)
    message(FATAL_ERROR "--press move=1,0 did not move the player (hash ${axis_HASH} equals the idle run)")
endif()
if(NOT axis_HASH STREQUAL axis1_HASH OR NOT axis_HASH STREQUAL axis4_HASH)
    message(FATAL_ERROR "axis run differs by job count: ${axis_HASH} ${axis1_HASH} ${axis4_HASH}")
endif()
if(mixed_HASH STREQUAL axis_HASH OR mixed_HASH STREQUAL base_HASH)
    message(FATAL_ERROR "a second axis entry (move=-1,0 from tick 91) had no effect: ${mixed_HASH}")
endif()
if(NOT unknown_HASH STREQUAL base_HASH)
    message(FATAL_ERROR "an unknown axis changed the state: ${unknown_HASH} vs ${base_HASH}")
endif()
if(NOT unknown_LOG MATCHES "--press: ignoring")
    message(FATAL_ERROR "an unknown axis was not reported:\n${unknown_LOG}")
endif()

# 2./3. exit codes: a copy of the project with a broken or a panicking script
set(broken "${OUT}/broken")
file(COPY "${PROJECT}/" DESTINATION "${broken}")
file(APPEND "${broken}/scripts/game.plm" "\nthis is not valid pluma )(\n")
run(compile --game "${broken}" --quit-after 1)
if(NOT compile_CODE EQUAL 2)
    message(FATAL_ERROR "a script that does not compile exited ${compile_CODE}, not 2:\n${compile_LOG}")
endif()

set(panicky "${OUT}/panicky")
file(COPY "${PROJECT}/" DESTINATION "${panicky}")
file(READ "${panicky}/scripts/game.plm" game)
string(REPLACE "fn ready() {" "fn ready() {\n        panic(\"boom\")" game "${game}")
file(WRITE "${panicky}/scripts/game.plm" "${game}")
run(panic --game "${panicky}" --quit-after 1)
if(NOT panic_CODE EQUAL 3)
    message(FATAL_ERROR "a script that panics exited ${panic_CODE}, not 3:\n${panic_LOG}")
endif()

# 4. the screenshot tick: 240 by default, --screenshot-tick overrides
run(shot --game "${PROJECT}" --screenshot "${OUT}/shot.ppm")
if(NOT shot_CODE EQUAL 0 OR NOT shot_LOG MATCHES "at tick 240 ")
    message(FATAL_ERROR "the default screenshot tick is not 240 (${shot_CODE}):\n${shot_LOG}")
endif()
if(NOT base_LOG MATCHES "at tick 150 ")
    message(FATAL_ERROR "--screenshot-tick 150 did not capture tick 120:\n${base_LOG}")
endif()

message(STATUS "cli automation ok (axis hash ${axis_HASH}, idle ${base_HASH})")
