# ImGui editor only (--imgui; the RmlUi editor has no OS-window undocking). Undockable panels (DESIGN.md 4.13, milestone 11.7b) through the shipped editor, GL on the
# offscreen SDL driver:
#   1. run A: undock Console into a second OS window, expect 1 extra window, save the layout
#   2. run B: a fresh editor with --layout finds the window restored (same position)
#   3. run C: without the layout there is no extra window; run D: --no-viewports keeps the panel inside
# Skipped (SKIPPED: in the output) when the platform cannot make a second offscreen window.
# Invoked by ctest with -DEDITOR=... -DPROJECT=... -DOUT=...

if(CMAKE_HOST_WIN32)
    message(STATUS "SKIPPED: the offscreen SDL driver is not used on Windows")
    return()
endif()
set(ENV{SDL_VIDEO_DRIVER} offscreen)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/project" "${OUT}/user")
file(COPY "${PROJECT}/project.toml" "${PROJECT}/scripts" DESTINATION "${OUT}/project")
set(ENV{CUERVO_USER_DIR} "${OUT}/user")

# A small editor window, so the undocked panel's window is outside it.
set(common --imgui --project "${OUT}/project" --backend gl --no-audio --size 700x500)

file(WRITE "${OUT}/a.cmds" "undock Console 760 120\nexpect-viewports 1\nsave-layout vp\n")
file(WRITE "${OUT}/b.cmds" "expect-viewports 1\nexpect-viewport Console 760 120\n")
file(WRITE "${OUT}/c.cmds" "expect-viewports 0\n")

function(run_editor name)
    execute_process(COMMAND "${EDITOR}" ${common} ${ARGN}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${name}_code ${code} PARENT_SCOPE)
    set(${name}_log "${out}\n${err}" PARENT_SCOPE)
endfunction()

run_editor(a --script-commands "${OUT}/a.cmds")
if(NOT a_code EQUAL 0)
    if(a_log MATCHES "SDL_GL_CreateContext failed|create_window_surface|cannot open a second window|could not open a window")
        message(STATUS "SKIPPED: this platform cannot make a second offscreen window:\n${a_log}")
        return()
    endif()
    message(FATAL_ERROR "run A (undock + save-layout) failed (${a_code}):\n${a_log}")
endif()
if(a_log MATCHES "\\[error\\]")
    message(FATAL_ERROR "errors while undocking:\n${a_log}")
endif()

run_editor(b --layout vp --script-commands "${OUT}/b.cmds")
if(NOT b_code EQUAL 0)
    message(FATAL_ERROR "the saved layout did not restore the undocked window (${b_code}):\n${b_log}")
endif()

run_editor(c --script-commands "${OUT}/c.cmds")
if(NOT c_code EQUAL 0)
    message(FATAL_ERROR "without the layout there should be no extra window (${c_code}):\n${c_log}")
endif()

file(WRITE "${OUT}/d.cmds" "undock Console 760 120\n")
run_editor(d --no-viewports --script-commands "${OUT}/d.cmds")
if(d_code EQUAL 0 OR NOT d_log MATCHES "cannot open a second window")
    message(FATAL_ERROR "--no-viewports should refuse undock (${d_code}):\n${d_log}")
endif()
message(STATUS "undocked panel restored from the saved layout")
