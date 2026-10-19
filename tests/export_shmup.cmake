# The release path end to end (docs/EXPORT.md, DESIGN.md 6 milestone 7):
#   1. copy examples/shmup to OUT/src and export it twice for Linux
#      (cuervo-tools export --platform linux) and twice with --aot
#      (ScriptForm::Native): each pair of game.pak files must be
#      byte-identical (deterministic packing)
#   2. delete the project copy and move only dist/ and dist-aot/ somewhere else
#   3. run both exported launchers from an unrelated working directory, headless
#      (GL, offscreen), with a private user directory: both must find game.pak
#      next to themselves and reach the dev state hash at tick 240
# Invoked by ctest with -DTOOLS=... -DPROJECT=... -DOUT=... -DEXPECT=<hash>

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/src" "${OUT}/cwd" "${OUT}/user")
file(COPY "${PROJECT}/" DESTINATION "${OUT}/src/shmup")

if(CMAKE_HOST_WIN32)
    set(PLATFORM windows)
    set(LAUNCHER_EXE "shmup.exe")
else()
    set(PLATFORM linux)
    set(LAUNCHER_EXE "shmup")
endif()

foreach(n 1 2)
    execute_process(
        COMMAND "${TOOLS}" export "${OUT}/src/shmup" --platform ${PLATFORM} --engine full --strip -o "${OUT}/dist${n}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "cuervo-tools export failed (${code}):\n${out}\n${err}")
    endif()
    execute_process(
        COMMAND "${TOOLS}" export "${OUT}/src/shmup" --platform ${PLATFORM} --engine full --aot --strip -o "${OUT}/dist_aot${n}"
        RESULT_VARIABLE code_aot OUTPUT_VARIABLE out_aot ERROR_VARIABLE err_aot)
    if(NOT code_aot EQUAL 0)
        message(FATAL_ERROR "cuervo-tools export --aot failed (${code_aot}):\n${out_aot}\n${err_aot}")
    endif()
endforeach()
message(STATUS "export: ${out}")
message(STATUS "export --aot: ${out_aot}")
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/dist1/game.pak" "${OUT}/dist2/game.pak"
                RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "exporting the same project twice gave different game.pak files")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/dist_aot1/game.pak" "${OUT}/dist_aot2/game.pak"
                RESULT_VARIABLE differs_aot)
if(NOT differs_aot EQUAL 0)
    message(FATAL_ERROR "exporting the same project twice with --aot gave different game.pak files")
endif()
if(NOT EXISTS "${OUT}/dist1/${LAUNCHER_EXE}" OR NOT EXISTS "${OUT}/dist_aot1/${LAUNCHER_EXE}")
    message(FATAL_ERROR "the export has no '${LAUNCHER_EXE}' launcher")
endif()
# The player's third-party notices travel with the game (THIRD_PARTY_NOTICES.md).
foreach(f licenses/THIRD_PARTY_NOTICES.md licenses/SDL3.txt)
    if(NOT EXISTS "${OUT}/dist1/${f}")
        message(FATAL_ERROR "the export is missing ${f}")
    endif()
endforeach()

# Only dist/ ships: the project is gone and the build is elsewhere.
file(REMOVE_RECURSE "${OUT}/src" "${OUT}/dist2" "${OUT}/dist_aot2")
file(RENAME "${OUT}/dist1" "${OUT}/shipped")
file(RENAME "${OUT}/dist_aot1" "${OUT}/shipped_aot")

if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
# An exported game never asks for the EULA (DESIGN.md 4.16): run it as a customer would, with nothing accepted.
unset(ENV{CUERVO_ACCEPT_EULA})
foreach(variant shipped shipped_aot)
    execute_process(
        COMMAND "${OUT}/${variant}/${LAUNCHER_EXE}" --backend gl --no-audio --screenshot "${variant}.ppm"
        WORKING_DIRECTORY "${OUT}/cwd"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(log "${out}${err}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "the exported game (${variant}) failed (${code}):\n${log}")
    endif()
    if(NOT log MATCHES "Pack '")
        message(FATAL_ERROR "the exported game (${variant}) did not run from its pack:\n${log}")
    endif()
    if(NOT log MATCHES "state hash ${EXPECT}")
        message(FATAL_ERROR "the exported game's (${variant}) state hash at tick 240 is not ${EXPECT}:\n${log}")
    endif()
    if(log MATCHES "\\[error\\]|panic|examples/")
        message(FATAL_ERROR "errors (or project paths) while running the exported game (${variant}):\n${log}")
    endif()
endforeach()
message(STATUS "exported shmup (source + AOT native) ran: state hash ${EXPECT}")
