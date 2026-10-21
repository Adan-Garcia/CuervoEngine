# Milestone 8.9: the fresh-checkout user path, from nothing to a shipped game (DESIGN.md 6, 4.11, 4.13;
# docs/GUIDE.md sections 1, 2, 7 and 9), through the shipped binaries only.
#   1. EULA: with nothing accepted, cuervo-editor and cuervo-tools refuse (non-zero, the message says how to
#      accept) and create nothing; CUERVO_ACCEPT_EULA=<version> accepts one run and stores nothing
#   2. create: `cuervo-editor --new-project` (what the welcome screen's New project does) makes a project in an
#      empty directory from nothing and refuses a directory that already has files
#   3. edit: an editor command script adds nodes, sets properties, saves the scene, plays it for TICK ticks
#      and prints the state hash H; the saved scene round-trips through `scene dump | scene build`; a second
#      session reproduces H from the saved file; `cuervo --game` (the dev run) reaches H
#   4. script: a script file is added to the project, a node of its class is placed, the new hash H2 is
#      reached in the editor and by `cuervo --game`; a hot reload mid-play keeps H2
#   5. export: bytecode and --aot (and --engine stripped when PLAYER_CACHE is given) are exported, the project
#      is deleted, only dist/ is launched from another directory with a private user dir and no EULA
#      acceptance: exit status 0 with the screenshot-tick mechanism (state hash H2) and with --quit-after
# Invoked by ctest with -DTOOLS= -DRUNTIME= -DEDITOR= -DVERSION=<EULA version> -DOUT= [-DTICK=90]
# [-DPLAYER_CACHE=<dir>]

if(NOT DEFINED TICK)
    set(TICK 90)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/user" "${OUT}/work")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
    set(NULL_DEVICE /dev/null)
    set(EXE_SUFFIX "")
    set(PLATFORM linux)
else()
    set(NULL_DEVICE NUL)
    set(EXE_SUFFIX ".exe")
    set(PLATFORM windows)
endif()
# Nothing here touches the real user: acceptance and recents would go to this directory.
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
unset(ENV{CUERVO_ACCEPT_EULA})
set(stored "${OUT}/user/eula-accepted.txt")
set(PROJECT "${OUT}/work/Workflow Game")

function(fail what out err)
    message(FATAL_ERROR "workflow: ${what}\n${out}\n${err}")
endfunction()

# Runs a command with the EULA accepted for this run only (what CI and scripts do); sets <log> to its output.
function(run_ok log what)
    set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err
                    INPUT_FILE ${NULL_DEVICE} TIMEOUT 300)
    unset(ENV{CUERVO_ACCEPT_EULA})
    if(NOT code EQUAL 0 OR "${out}${err}" MATCHES "\\[error\\]|panic")
        fail("${what} failed (${code})" "${out}" "${err}")
    endif()
    set(${log} "${out}${err}" PARENT_SCOPE)
endfunction()

# The state hash a log reports at tick N ("at tick N (state hash H)"), from the editor or a game.
function(hash_at result log tick)
    string(REGEX MATCH "at tick ${tick} \\(state hash ([0-9a-f]+)\\)" m "${log}")
    if(CMAKE_MATCH_1 STREQUAL "")
        fail("no state hash at tick ${tick} in the log" "${log}" "")
    endif()
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# 1. the EULA gate on the way in ------------------------------------------------------
foreach(refused "${EDITOR}|--new-project|${OUT}/refused" "${TOOLS}|tier|check")
    string(REPLACE "|" ";" refused "${refused}")
    execute_process(COMMAND ${refused} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err
                    INPUT_FILE ${NULL_DEVICE} TIMEOUT 60)
    if(code EQUAL 0 OR NOT err MATCHES "--accept-eula" OR NOT err MATCHES "CUERVO_ACCEPT_EULA=${VERSION}")
        fail("an unaccepted EULA was not refused with the way to accept (${refused}, ${code})" "${out}" "${err}")
    endif()
endforeach()
if(EXISTS "${OUT}/refused" OR EXISTS "${stored}")
    fail("a refused run created a project or stored an acceptance" "" "")
endif()

# 2. create the project from nothing --------------------------------------------------------
run_ok(log "new project" "${EDITOR}" --new-project "${PROJECT}" --template walker --name "Workflow Game")
foreach(f project.toml scripts/walker.plm scenes/main.scene)
    if(NOT EXISTS "${PROJECT}/${f}")
        fail("the new project has no ${f}" "${log}" "")
    endif()
endforeach()
file(READ "${PROJECT}/project.toml" toml)
if(NOT toml MATCHES "name = \"Workflow Game\"")
    fail("project.toml does not carry the name:\n${toml}" "" "")
endif()
set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
execute_process(COMMAND "${EDITOR}" --new-project "${PROJECT}" RESULT_VARIABLE code OUTPUT_VARIABLE out
                ERROR_VARIABLE err INPUT_FILE ${NULL_DEVICE})
unset(ENV{CUERVO_ACCEPT_EULA})
if(code EQUAL 0 OR NOT err MATCHES "already has files")
    fail("a second --new-project into the same folder was not refused (${code})" "${out}" "${err}")
endif()
set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
execute_process(COMMAND "${EDITOR}" --new-project "${OUT}/work/other" --template nope RESULT_VARIABLE code
                OUTPUT_VARIABLE out ERROR_VARIABLE err INPUT_FILE ${NULL_DEVICE})
unset(ENV{CUERVO_ACCEPT_EULA})
if(code EQUAL 0 OR NOT err MATCHES "no template" OR EXISTS "${OUT}/work/other")
    fail("an unknown template was not refused cleanly (${code})" "${out}" "${err}")
endif()
if(EXISTS "${stored}")
    fail("CUERVO_ACCEPT_EULA stored an acceptance (it is for one run)" "" "")
endif()

# 3. edit, save, play ------------------------------------------------------------------------
set(edit_cmds
    "open scenes/main.scene\n"
    "add / Node2D decor\n"
    "add /decor Walker buddy\n"
    "set /decor/buddy position (200.0, 150.0)\n"
    "set /decor/buddy speed 120.0\n"
    "set /walker color \"ffcc00\"\n"
    "rename /decor/buddy pal\n"
    "undo\n"
    "redo\n"
    "expect /walker color \"ffcc00\"\n"
    "expect /decor/pal speed 120.0\n"
    "save\n"
    "expect-clean\n"
    "play\n"
    "step ${TICK}\n"
    "expect-tick ${TICK}\n"
    "print-hash\n"
    "stop\n")
string(CONCAT edit_cmds ${edit_cmds})
file(WRITE "${OUT}/edit.cmds" "${edit_cmds}")
run_ok(log "the edit session" "${EDITOR}" --project "${PROJECT}" --no-audio --script-commands "${OUT}/edit.cmds")
hash_at(H "${log}" ${TICK})
set(ENV{CUERVO_ACCEPT_EULA} "${VERSION}")
execute_process(COMMAND "${TOOLS}" scene dump "${PROJECT}/scenes/main.scene" OUTPUT_FILE "${OUT}/main.txt"
                RESULT_VARIABLE code ERROR_VARIABLE err)
file(READ "${OUT}/main.txt" dump)
if(NOT code EQUAL 0 OR NOT dump MATCHES "node Walker \"pal\"" OR NOT dump MATCHES "speed: float = 120\\.0"
   OR NOT dump MATCHES "r = 1\\.0, g = 0\\.8")
    fail("the saved scene lacks the edits (${code}):\n${dump}" "" "${err}")
endif()
execute_process(COMMAND "${TOOLS}" scene build "${OUT}/main.txt" "${OUT}/main.scene" RESULT_VARIABLE code
                ERROR_VARIABLE err)
unset(ENV{CUERVO_ACCEPT_EULA})
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/main.scene" "${PROJECT}/scenes/main.scene"
                RESULT_VARIABLE differs)
if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
    fail("the saved scene does not round-trip through the text view (${code})" "" "${err}")
endif()

# A second session plays the saved file to the same hash (nothing but the file carries the edits).
file(WRITE "${OUT}/again.cmds" "play\nstep ${TICK}\nexpect-hash ${H}\nstop\n")
run_ok(log "replaying the saved scene" "${EDITOR}" --project "${PROJECT}" --no-audio --script-commands "${OUT}/again.cmds")

set(ENV{CUERVO_SCREENSHOT_TICK} "${TICK}")
run_ok(log "the dev run" "${RUNTIME}" --game "${PROJECT}" --backend gl --no-audio --screenshot "${OUT}/dev.ppm")
hash_at(dev_hash "${log}" ${TICK})
if(NOT dev_hash STREQUAL H)
    fail("`cuervo --game` ended at ${dev_hash}, the editor's play at ${H}" "${log}" "")
endif()

# 4. a script file in the new project, and a hot reload ---------------------------------------
file(WRITE "${PROJECT}/scripts/pulse.plm"
"// Added by hand to the new project: a dot that grows and shrinks.
class Pulse extends Node2D {
    @export var base = 10.0
    // Script globals are what the state hash covers (a node's own fields are not), so the dot reports where
    // it has been; a different scene or script gives another hash.
    static var trail = 0
    var age = 0

    fn update(dt: float) {
        age += 1
        position.x += 1.0
        Pulse.trail += age + (position.x as int)
    }

    fn draw(g: Canvas) {
        g.circle(position, base + (age % 20) as float, fill = Color.hex(\"ff4060\"))
    }
}
")
set(script_cmds
    "open scenes/main.scene\n"
    "add / Pulse pulse\n"
    "set /pulse position (100.0, 600.0)\n"
    "save\n"
    "play\n"
    "step ${TICK}\n"
    "print-hash\n"
    "stop\n")
string(CONCAT script_cmds ${script_cmds})
file(WRITE "${OUT}/script.cmds" "${script_cmds}")
run_ok(log "the script session" "${EDITOR}" --project "${PROJECT}" --no-audio --script-commands "${OUT}/script.cmds")
hash_at(H2 "${log}" ${TICK})
if(H2 STREQUAL H)
    fail("adding a node with a script did not change the state hash (${H})" "${log}" "")
endif()
run_ok(log "the dev run with the script" "${RUNTIME}" --game "${PROJECT}" --backend gl --no-audio
       --screenshot "${OUT}/dev2.ppm")
hash_at(dev_hash2 "${log}" ${TICK})
if(NOT dev_hash2 STREQUAL H2)
    fail("`cuervo --game` ended at ${dev_hash2} with the script, the editor at ${H2}" "${log}" "")
endif()
# A comment appended to the script and a forced reload mid-play change nothing (DESIGN.md 5.13).
math(EXPR first "${TICK} / 3")
math(EXPR rest "${TICK} - ${first}")
file(COPY "${PROJECT}/" DESTINATION "${OUT}/reload")
file(WRITE "${OUT}/reload.cmds"
     "play\nstep ${first}\nappend-file scripts/pulse.plm // edited while playing\nreload\nstep ${rest}\n"
     "expect-tick ${TICK}\nexpect-hash ${H2}\nstop\n")
run_ok(log "the hot reload" "${EDITOR}" --project "${OUT}/reload" --no-audio --script-commands "${OUT}/reload.cmds")

# 5. export, ship alone, launch elsewhere -------------------------------------------------------
set(variants bytecode aot)
if(DEFINED PLAYER_CACHE AND NOT PLAYER_CACHE STREQUAL "")
    list(APPEND variants stripped)
endif()
foreach(variant ${variants})
    if(variant STREQUAL "bytecode")
        set(export_args --engine full --strip)
    elseif(variant STREQUAL "aot")
        set(export_args --engine full --aot --strip)
    else()
        set(export_args --engine stripped --strip --player-cache "${PLAYER_CACHE}")
    endif()
    run_ok(log "export (${variant})" "${TOOLS}" export "${PROJECT}" --platform ${PLATFORM} ${export_args}
           -o "${OUT}/dist_${variant}")
    set(launcher "${OUT}/dist_${variant}/workflow-game${EXE_SUFFIX}")
    if(NOT EXISTS "${launcher}" OR NOT EXISTS "${OUT}/dist_${variant}/game.pak")
        fail("export (${variant}) lacks the launcher or game.pak (named after the project)" "${log}" "")
    endif()
endforeach()
# Only dist/ ships: the project is gone.
file(REMOVE_RECURSE "${PROJECT}" "${OUT}/reload")
foreach(variant ${variants})
    set(launcher "${OUT}/dist_${variant}/workflow-game${EXE_SUFFIX}")
    file(MAKE_DIRECTORY "${OUT}/cwd_${variant}" "${OUT}/user_${variant}")
    set(ENV{CUERVO_USER_DIR} "${OUT}/user_${variant}")
    unset(ENV{CUERVO_ACCEPT_EULA}) # an exported game never asks for the EULA (DESIGN.md 4.16)
    execute_process(COMMAND "${launcher}" --backend gl --no-audio --screenshot "${OUT}/${variant}.ppm"
                    WORKING_DIRECTORY "${OUT}/cwd_${variant}" INPUT_FILE ${NULL_DEVICE} TIMEOUT 120
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(log "${out}${err}")
    if(NOT code EQUAL 0 OR NOT log MATCHES "Pack '" OR log MATCHES "\\[error\\]|panic|${OUT}/work")
        fail("the exported game (${variant}) failed, did not run from its pack or named the project (${code})"
             "${out}" "${err}")
    endif()
    if(NOT variant STREQUAL "stripped") # a stripped player's hash covers fewer preludes (DESIGN.md 4.11)
        hash_at(shipped_hash "${log}" ${TICK})
        if(NOT shipped_hash STREQUAL H2)
            fail("the exported game (${variant}) reached ${shipped_hash}, the editor's play ${H2}" "${out}" "${err}")
        endif()
    endif()
    # A plain run that quits by itself leaves with status 0 too.
    unset(ENV{CUERVO_SCREENSHOT_TICK})
    execute_process(COMMAND "${launcher}" --backend gl --no-audio --quit-after 0.3
                    WORKING_DIRECTORY "${OUT}/cwd_${variant}" INPUT_FILE ${NULL_DEVICE} TIMEOUT 120
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(ENV{CUERVO_SCREENSHOT_TICK} "${TICK}")
    if(NOT code EQUAL 0)
        fail("the exported game (${variant}) did not exit 0 after --quit-after (${code})" "${out}" "${err}")
    endif()
endforeach()
if(EXISTS "${stored}" OR EXISTS "${OUT}/user_bytecode/eula-accepted.txt")
    fail("an acceptance was stored during the workflow" "" "")
endif()
message(STATUS "workflow: new project, edit + save + play (${H}), script + reload (${H2}), exports [${variants}] launched elsewhere agree")
