# The RmlUi editor (--rml-ui) honors the start-up switches the ImGui editor has, on offscreen GL:
# --select, --tool, --tabs, --layout, --palette, --screen welcome, --script-commands with --screenshot,
# and rejects nothing silently (unknown names are warned about). Exit 0 and a screenshot each time.
# Invoked by ctest with -DEDITOR=... -DPROJECT=... -DOUT=...
if(CMAKE_HOST_WIN32)
    message(STATUS "SKIPPED: the offscreen SDL driver is not used on Windows")
    return()
endif()
set(ENV{SDL_VIDEO_DRIVER} offscreen)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/user")
file(COPY "${PROJECT}/project.toml" "${PROJECT}/level.cmds" "${PROJECT}/scripts" "${PROJECT}/scenes" DESTINATION "${OUT}/project")
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
set(common --rml-ui --project "${OUT}/project" --backend gl --no-audio --no-splash --size 900x600)

function(run name)
    execute_process(COMMAND "${EDITOR}" ${common} ${ARGN} --screenshot "${OUT}/${name}.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT EXISTS "${OUT}/${name}.ppm")
        message(FATAL_ERROR "${name}: exit ${code}\n${out}\n${err}")
    endif()
    set(${name}_log "${out}\n${err}" PARENT_SCOPE)
endfunction()

run(a --select /ship,/left --tool rotate --tabs Debugger,Profiler --palette zoom)
if(a_log MATCHES "--select: no node|--tabs: no panel")
    message(FATAL_ERROR "valid switches were refused:\n${a_log}")
endif()
run(b --select /nowhere --tabs Nothing --layout missing --screen welcome)
foreach(expect "--select: no node at '/nowhere'" "--tabs: no panel 'Nothing'" "--layout: no saved layout 'missing'")
    string(FIND "${b_log}" "${expect}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "missing warning ${expect}:\n${b_log}")
    endif()
endforeach()
# A script plus a screenshot: the script runs first (headless), then the window opens on the result.
file(WRITE "${OUT}/s.cmds" "open scenes/level.scene\nadd / Node2D probe\n")
run(c --script-commands "${OUT}/s.cmds")
if(NOT c_log MATCHES "2 command\\(s\\) applied")
    message(FATAL_ERROR "the script did not run before the window opened:\n${c_log}")
endif()
# Browse... without a system dialog (offscreen): says so and the typed path still works.
run(d --ui-events "key ctrl+shift+s|move 292,234|click 292,234|move 300,300")
string(FIND "${d_log}" "native file dialog unavailable" at)
if(at EQUAL -1)
    message(FATAL_ERROR "Browse... did not report the missing dialog:\n${d_log}")
endif()
message(STATUS "rml start-up switches ok")
