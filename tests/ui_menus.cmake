cmake_policy(SET CMP0007 NEW)

# Milestone 9.3's "done when" (DESIGN.md 4.17, 6): the settings menu and
# inventory of examples/ui_menus, built from the UI widgets, are driven by
# keyboard, gamepad and pointer, and replay. End to end through the shipped
# binaries, for three scripted runs:
#   keyboard: arrows/Enter/Page keys (ui_* actions, typed text through --type)
#   gamepad:  what a pad sends (d-pad, A/B, shoulders, right stick) as the
#             actions it is bound to
#   pointer:  --pointer moves the mouse, ui_click presses (hover, clicks, wheel,
#             dragging a scroll area and the split bar)
#   1. `cuervo --game` runs the scenario headless (GL, offscreen) to a given
#      tick, records it (--record), screenshots it, and logs the state hash
#   2. that hash is the pinned one (update it with the log's when the example or
#      the UI changes on purpose)
#   3. `cuervo-editor --replay-verify` replays the recording on 1 and 8 job
#      threads: every tick's state hash (menu state included, examples/ui_menus
#      keeps it in numeric statics) must match, and end at the same hash
#   4. a replay of a drifted project (an extra static) must be refused
# Controls have explicit sizes in the example, so what a recorded click hits
# doesn't depend on the font (the verifier may have none).
# Invoked by ctest with -DEDITOR=... -DRUNTIME=... -DPROJECT=... -DOUT=...

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()

# name; screenshot tick; --press; --pointer; --type; pinned state hash
set(scenarios
    "keyboard|120|5:ui_down,10:ui_down,15:ui_right,18:ui_right,21:ui_left,24:ui_down,27:ui_left,30:ui_tab_next,33:ui_down,36:ui_down,39:ui_accept,47:ui_accept,50:ui_down,53:ui_accept,56:ui_down,59:ui_accept,62:ui_down,65:ui_accept,68:ui_down,71:ui_accept,74:ui_down,77:ui_accept,80:ui_left,83:ui_accept,86:ui_tab_next,90:ui_up,93:ui_down,96:ui_down,99:ui_down,102:ui_accept,105:ui_accept||42:Ada|c1b368e077a3e037"
    "gamepad|125|5:ui_down,10:ui_tab_next,13:ui_tab_next,16:ui_down,19:ui_down,22:ui_up,25-35:ui_scroll_down,38:ui_accept,41:ui_down,44:ui_accept,47:ui_tab_prev,50:ui_down,53:ui_down,56:ui_accept,59:ui_cancel,62:ui_cancel,65:ui_down,68:ui_accept,71:ui_cancel,74:ui_down,77:ui_accept,80:ui_tab_prev,83:ui_down,86:ui_down,89:ui_right,92:ui_right,95:ui_down,98:ui_left,101:ui_cancel,104:ui_down,107:ui_accept,110:ui_cancel,113:ui_accept,116:ui_left,119:ui_accept|||32a5ab67471ed4b8"
    "pointer|175|10:ui_click,20:ui_click,30:ui_click,40:ui_click,50:ui_click,55:ui_click,60:ui_click,66:ui_click,70:ui_click,75-80:ui_scroll_down,85-95:ui_click,100:ui_click,110:ui_click,125-135:ui_click|1:336:170,19:578:222,29:578:282,39:570:484,49:578:342,54:578:402,59:188:628,65:388:400,69:490:170,72:300:400,84:201:300,90:201:400,99:348:234,109:400:266,120:152:234,124:654:300,130:754:300,136:152:234|24:Zed|14103efd61217a67"
)

function(run_scenario spec)
    string(REPLACE "|" ";" parts "${spec}")
    list(GET parts 0 name)
    list(GET parts 1 tick)
    list(GET parts 2 press)
    # Empty fields vanish from a CMake list: split by hand.
    string(REGEX MATCH "^[^|]*\\|[^|]*\\|[^|]*\\|([^|]*)\\|([^|]*)\\|([^|]*)$" m "${spec}")
    set(pointer "${CMAKE_MATCH_1}")
    set(typed "${CMAKE_MATCH_2}")
    set(pinned "${CMAKE_MATCH_3}")
    set(args --game "${PROJECT}" --backend gl --no-audio --press "${press}"
             --record "${OUT}/${name}.replay" --screenshot "${OUT}/${name}.ppm")
    if(NOT pointer STREQUAL "")
        list(APPEND args --pointer "${pointer}")
    endif()
    if(NOT typed STREQUAL "")
        list(APPEND args --type "${typed}")
    endif()
    set(ENV{CUERVO_SCREENSHOT_TICK} "${tick}")
    execute_process(COMMAND "${RUNTIME}" ${args}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX MATCH "at tick ${tick} \\(state hash ([0-9a-f]+)\\)" m2 "${out}${err}")
    set(hash "${CMAKE_MATCH_1}")
    if(NOT code EQUAL 0 OR hash STREQUAL "" OR NOT EXISTS "${OUT}/${name}.replay")
        message(FATAL_ERROR "${name}: cuervo --game --record failed (${code}):\n${out}\n${err}")
    endif()
    if("${out}${err}" MATCHES "\\[error\\]|panic")
        message(FATAL_ERROR "${name}: errors while running:\n${out}\n${err}")
    endif()
    if(NOT hash STREQUAL pinned)
        message(FATAL_ERROR "${name}: state hash ${hash} at tick ${tick}, expected ${pinned}")
    endif()
    foreach(jobs 1 8)
        execute_process(
            COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/${name}.replay" --jobs ${jobs}
            RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "${tick} ticks replayed on ${jobs} thread")
            message(FATAL_ERROR "${name}: --replay-verify on ${jobs} job(s) failed (${code}):\n${out}\n${err}")
        endif()
        string(REGEX MATCH "final state hash ([0-9a-f]+)" m3 "${out}${err}")
        if(NOT CMAKE_MATCH_1 STREQUAL hash)
            message(FATAL_ERROR "${name}: --replay-verify ended at ${CMAKE_MATCH_1}, the run at ${hash}")
        endif()
    endforeach()
    # The same recording against scripts that drifted must fail at tick 1.
    file(COPY "${PROJECT}/" DESTINATION "${OUT}/drifted_${name}")
    file(APPEND "${OUT}/drifted_${name}/scripts/main.plm" "\nclass Drift {\n    static var amount = 1\n}\n")
    execute_process(
        COMMAND "${EDITOR}" --project "${OUT}/drifted_${name}" --replay-verify "${OUT}/${name}.replay" --jobs 1
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "mismatch at tick 1 of ${tick}")
        message(FATAL_ERROR "${name}: --replay-verify accepted a drifted project (${code}):\n${out}\n${err}")
    endif()
    message(STATUS "${name}: ${tick} ticks, state hash ${hash}; the replay verifies on 1 and 8 threads")
endfunction()

foreach(spec IN LISTS scenarios)
    run_scenario("${spec}")
endforeach()
