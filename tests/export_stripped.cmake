# A stripped export end to end (milestone 7.5, docs/EXPORT.md "Stripped
# players"); slow because it configures and builds a Release engine the
# first time (the player cache in PLAYER_CACHE is kept between runs):
#   1. export a copy of PROJECT with --engine stripped (the export itself
#      runs the player headless and compares it with the full engine)
#   2. delete the project copy, move only dist/, run the launcher from an
#      unrelated directory: it must run from game.pak
#   3. its state hash must equal the stripped player's dev run (`--game` on
#      the project) and the full player's dev run: the hash doesn't depend on
#      the features a player was built with (ScriptHost::state_hash); the
#      frame at tick 240 must equal the full player's too
#   4. the shipped player must be smaller than the full one
# Invoked by ctest with -DTOOLS=... -DRUNTIME=<full player> -DPROJECT=...
# -DNAME=<launcher> -DOUT=... -DPLAYER_CACHE=...

if(CMAKE_HOST_WIN32)
    set(PLATFORM windows)
    set(EXE_SUFFIX ".exe")
else()
    set(PLATFORM linux)
    set(EXE_SUFFIX "")
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/src" "${OUT}/cwd" "${OUT}/user")
file(COPY "${PROJECT}/" DESTINATION "${OUT}/src/game")
file(COPY "${PROJECT}/" DESTINATION "${OUT}/dev/game")

execute_process(
    COMMAND "${TOOLS}" export "${OUT}/src/game" --platform ${PLATFORM} --engine stripped --strip -o "${OUT}/dist"
            --player-cache "${PLAYER_CACHE}"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-tools export --engine stripped failed (${code}):\n${out}\n${err}")
endif()
message(STATUS "export: ${out}")
if(NOT out MATCHES "player: stripped, ([0-9]+) bytes")
    message(FATAL_ERROR "the export did not ship a stripped player:\n${out}")
endif()
set(stripped_bytes "${CMAKE_MATCH_1}")
if(NOT out MATCHES "verified: state hash")
    message(FATAL_ERROR "the export did not verify its player:\n${out}")
endif()

file(REMOVE_RECURSE "${OUT}/src")
file(RENAME "${OUT}/dist" "${OUT}/shipped")

if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
set(ENV{CUERVO_SCREENSHOT_TICK} 240)
execute_process(
    COMMAND "${OUT}/shipped/${NAME}${EXE_SUFFIX}" --backend gl --no-audio --screenshot "${OUT}/shipped.ppm"
    WORKING_DIRECTORY "${OUT}/cwd"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(log "${out}${err}")
if(NOT code EQUAL 0 OR NOT log MATCHES "Pack '" OR log MATCHES "\\[error\\]|panic")
    message(FATAL_ERROR "the shipped stripped game failed (${code}):\n${log}")
endif()
if(NOT log MATCHES "state hash ([0-9a-f]+)")
    message(FATAL_ERROR "no state hash from the shipped game:\n${log}")
endif()
set(shipped_hash "${CMAKE_MATCH_1}")

# The same player on the project directory (it has no font importer, so the
# default TTF is an error there; the hash doesn't depend on it).
execute_process(
    COMMAND "${OUT}/shipped/${NAME}${EXE_SUFFIX}" --game "${OUT}/dev/game" --backend gl --no-audio --screenshot "${OUT}/dev.ppm"
    WORKING_DIRECTORY "${OUT}/cwd"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(log "${out}${err}")
if(NOT code EQUAL 0 OR NOT log MATCHES "state hash ([0-9a-f]+)")
    message(FATAL_ERROR "the stripped player's dev run failed (${code}):\n${log}")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL shipped_hash)
    message(FATAL_ERROR "the shipped game reaches ${shipped_hash}, the stripped player's dev run ${CMAKE_MATCH_1}")
endif()

# The full player's dev run draws the same frame.
execute_process(
    COMMAND "${RUNTIME}" --game "${OUT}/dev/game" --backend gl --no-audio --screenshot "${OUT}/full.ppm"
    WORKING_DIRECTORY "${OUT}/cwd"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "state hash ([0-9a-f]+)")
    message(FATAL_ERROR "the full player's dev run failed (${code}):\n${out}${err}")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL shipped_hash)
    message(FATAL_ERROR "the shipped game reaches ${shipped_hash}, the full player's dev run ${CMAKE_MATCH_1}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/shipped.ppm" "${OUT}/full.ppm"
                RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "the stripped export's frame at tick 240 differs from the full player's dev run")
endif()

file(SIZE "${RUNTIME}" full_bytes)
if(NOT stripped_bytes LESS full_bytes)
    message(FATAL_ERROR "the stripped player (${stripped_bytes} bytes) is not smaller than the full one (${full_bytes})")
endif()
message(STATUS "stripped export ran: state hash ${shipped_hash} (= its dev run and the full player's), frame = full dev run, "
               "${stripped_bytes} bytes vs ${full_bytes}")
