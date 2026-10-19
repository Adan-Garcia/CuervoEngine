# "A level can be built without code" (DESIGN.md 6, milestone 6), end to
# end through the shipped binaries:
#   1. copy examples/editor_level (scripts + project.toml, no scene) to OUT
#   2. cuervo-editor --script-commands level.cmds builds and saves the level
#   3. the result must equal the checked-in scenes/level.scene byte for byte
#   4. cuervo --game OUT runs it headless (GL, offscreen) to tick 240
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DPROJECT=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
file(COPY "${PROJECT}/project.toml" "${PROJECT}/level.cmds" "${PROJECT}/scripts" DESTINATION "${OUT}")

execute_process(
    COMMAND "${EDITOR}" --project "${OUT}" --script-commands "${OUT}/level.cmds"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "cuervo-editor --script-commands failed (${code}):\n${out}\n${err}")
endif()
if(NOT EXISTS "${OUT}/scenes/level.scene")
    message(FATAL_ERROR "the command script did not save scenes/level.scene:\n${out}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/scenes/level.scene" "${PROJECT}/scenes/level.scene"
                RESULT_VARIABLE differs)
if(NOT differs EQUAL 0)
    message(FATAL_ERROR "the editor-built level differs from examples/editor_level/scenes/level.scene")
endif()

if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()
execute_process(
    COMMAND "${RUNTIME}" --game "${OUT}" --backend gl --no-audio --screenshot "${OUT}/level.ppm"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0 OR NOT out MATCHES "state hash")
    message(FATAL_ERROR "cuervo --game failed on the editor-built level (${code}):\n${out}\n${err}")
endif()
if(err MATCHES "\\[error\\]|panic")
    message(FATAL_ERROR "errors while running the editor-built level:\n${err}")
endif()
message(STATUS "editor-built level ran: ${out}")
