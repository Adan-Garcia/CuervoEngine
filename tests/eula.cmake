# The EULA gate end to end through the shipped binaries (DESIGN.md 4.16 "EULA agreement"):
#   1. with no acceptance and no terminal, cuervo and cuervo-tools refuse and say how to accept
#   2. --accept-eula accepts and stores the version and date for this user; afterwards
#      neither program needs the flag
#   3. an acceptance of another version asks again
#   4. CUERVO_ACCEPT_EULA=<version> accepts one run without storing anything, another value refuses
#   5. running a pack never asks: `cuervo --pack game.pak`, and a game.pak next to the player
# Invoked by ctest with -DTOOLS=... -DRUNTIME=... -DPROJECT=... -DVERSION=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/user" "${OUT}/player")
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
    set(NULL_DEVICE /dev/null)
else()
    set(NULL_DEVICE NUL)
endif()
unset(ENV{CUERVO_ACCEPT_EULA})
set(stored "${OUT}/user/eula-accepted.txt")

function(run_expect want_ok text)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err
                    INPUT_FILE ${NULL_DEVICE} TIMEOUT 60)
    if(want_ok AND NOT code EQUAL 0)
        message(FATAL_ERROR "expected success (${text}): ${ARGN}\n${code}\n${out}\n${err}")
    endif()
    if(NOT want_ok)
        if(code EQUAL 0)
            message(FATAL_ERROR "expected a refusal (${text}): ${ARGN}\n${out}\n${err}")
        endif()
        if(NOT err MATCHES "--accept-eula" OR NOT err MATCHES "CUERVO_ACCEPT_EULA=${VERSION}" OR NOT err MATCHES "no work was lost")
            message(FATAL_ERROR "the refusal does not follow the message formula (${text}):\n${err}")
        endif()
    endif()
endfunction()

set(game_args --game "${PROJECT}" --backend gl --no-audio --quit-after 0.3)

# 1. refused
run_expect(FALSE "tools, no acceptance" "${TOOLS}" tier check "${PROJECT}")
run_expect(FALSE "runtime, no acceptance" "${RUNTIME}" ${game_args})
if(EXISTS "${stored}")
    message(FATAL_ERROR "a refused run stored an acceptance")
endif()

# 4. the environment variable accepts a run without storing it
set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
run_expect(TRUE "env" "${TOOLS}" tier check "${PROJECT}")
if(EXISTS "${stored}")
    message(FATAL_ERROR "CUERVO_ACCEPT_EULA stored an acceptance (it is for this run only)")
endif()
set(ENV{CUERVO_ACCEPT_EULA} "99.9")
execute_process(COMMAND "${TOOLS}" tier check "${PROJECT}" RESULT_VARIABLE code ERROR_VARIABLE err INPUT_FILE /dev/null)
if(code EQUAL 0 OR NOT err MATCHES "does not match")
    message(FATAL_ERROR "a wrong CUERVO_ACCEPT_EULA version was accepted:\n${err}")
endif()
unset(ENV{CUERVO_ACCEPT_EULA})

# 2. --accept-eula stores it
run_expect(TRUE "flag" "${TOOLS}" --accept-eula tier check "${PROJECT}")
file(READ "${stored}" content)
if(NOT content MATCHES "version=${VERSION}" OR NOT content MATCHES "accepted=20[0-9][0-9]-[01][0-9]-[0-3][0-9]")
    message(FATAL_ERROR "the stored acceptance lacks the version and date:\n${content}")
endif()
run_expect(TRUE "stored, tools" "${TOOLS}" tier check "${PROJECT}")
run_expect(TRUE "stored, runtime" "${RUNTIME}" ${game_args})

# 3. a new EULA version asks again
file(WRITE "${stored}" "version=0.0\naccepted=2000-01-01\n")
run_expect(FALSE "older version" "${TOOLS}" tier check "${PROJECT}")

# 5. packs never ask
set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
run_expect(TRUE "pack" "${TOOLS}" pack "${PROJECT}" -o "${OUT}/game.pak")
unset(ENV{CUERVO_ACCEPT_EULA})
file(REMOVE "${stored}")
run_expect(TRUE "--pack" "${RUNTIME}" --pack "${OUT}/game.pak" --backend gl --no-audio --quit-after 0.3)
file(COPY "${RUNTIME}" DESTINATION "${OUT}/player")
file(COPY "${OUT}/game.pak" DESTINATION "${OUT}/player")
get_filename_component(exe "${RUNTIME}" NAME)
run_expect(TRUE "game.pak next to the player" "${OUT}/player/${exe}" --backend gl --no-audio --quit-after 0.3)
if(EXISTS "${stored}")
    message(FATAL_ERROR "running a pack stored an acceptance")
endif()
message(STATUS "EULA gate: refuses without acceptance, stores the version, asks again for a new one, never asks for packs")
