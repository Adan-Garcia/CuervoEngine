# Milestone 8.7's stabilization sweep (DESIGN.md 6, 4.12, 4.13): one example, every
# release-path workflow, through the shipped binaries. Run once per example
# (tests/CMakeLists.txt registers examples_sweep_<name>), so ctest parallelizes them.
#   1. dev run: `cuervo --game` headless (GL, offscreen) with scripted --press input to
#      TICK, screenshot + state hash H; the same on --jobs 1 and --jobs 4 (equal hashes)
#   2. scenes: every scenes/**/*.scene goes through `scene dump | scene build` (same
#      bytes, same text) and `scene upgrade` (a current-version file is unchanged)
#   3. replay: the run is recorded (--record); cuervo-editor --replay-verify replays it
#      on 1 and 4 job threads (the final hash is H); a drifted project is refused
#   4. hot reload (editor --script-commands, `reload`): a project script is edited and
#      reloaded mid-play and the run still reaches the unreloaded hash; the same while
#      recording, and that replay verifies against the unedited project
#   5. exports: bytecode and --aot (and --engine stripped when PLAYER_CACHE is given)
#      are shipped alone, launched from another directory with a private user dir
#      and the same --press: same hash H, a screenshot of the same size as the dev run
# Invoked by ctest with -DTOOLS= -DRUNTIME= -DEDITOR= -DPROJECT= -DNAME= -DOUT= -DPRESS=
# [-DTICK=240] [-DRELOAD_EXACT=ON] [-DPLAYER_CACHE=<dir>]

if(NOT DEFINED TICK)
    set(TICK 240)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(NOT CMAKE_HOST_WIN32)
    set(ENV{SDL_VIDEO_DRIVER} offscreen)
endif()
set(ENV{CUERVO_SCREENSHOT_TICK} "${TICK}")
if(CMAKE_HOST_WIN32)
    set(EXE_SUFFIX ".exe")
    set(PLATFORM windows)
else()
    set(EXE_SUFFIX "")
    set(PLATFORM linux)
endif()

set(press_args "")
if(NOT PRESS STREQUAL "")
    set(press_args --press "${PRESS}")
endif()

function(fail what out err)
    message(FATAL_ERROR "${NAME}: ${what}\n${out}\n${err}")
endfunction()

# Runs a game binary; sets <result> to the state hash at TICK. ARGN: more arguments.
function(run_game result exe label)
    execute_process(
        COMMAND "${exe}" ${ARGN} --backend gl --no-audio --screenshot "${OUT}/${label}.ppm"
        WORKING_DIRECTORY "${OUT}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX MATCH "at tick ${TICK} \\(state hash ([0-9a-f]+)\\)" m "${out}${err}")
    set(found "${CMAKE_MATCH_1}")
    if(NOT code EQUAL 0 OR found STREQUAL "")
        fail("${label} failed (${code})" "${out}" "${err}")
    endif()
    if("${out}${err}" MATCHES "\\[error\\]|panic|\\[warn")
        fail("${label}: problems while running" "${out}" "${err}")
    endif()
    if(NOT EXISTS "${OUT}/${label}.ppm")
        fail("${label}: no screenshot" "${out}" "${err}")
    endif()
    set(${result} "${found}" PARENT_SCOPE)
endfunction()

# 1. dev run, on 1 and 4 job threads, recording ------------------------------------
run_game(hash "${RUNTIME}" dev --game "${PROJECT}" ${press_args} --jobs 1 --record "${OUT}/run.replay")
run_game(hash4 "${RUNTIME}" dev4 --game "${PROJECT}" ${press_args} --jobs 4)
if(NOT hash STREQUAL hash4)
    fail("state hash at tick ${TICK} differs between --jobs 1 (${hash}) and --jobs 4 (${hash4})" "" "")
endif()
if(NOT EXISTS "${OUT}/run.replay")
    fail("--record wrote no replay" "" "")
endif()

# 2. scenes ---------------------------------------------------------------------
file(GLOB_RECURSE scenes "${PROJECT}/*.scene")
set(scene_count 0)
foreach(scene ${scenes})
    math(EXPR scene_count "${scene_count} + 1")
    set(base "${OUT}/scene${scene_count}")
    execute_process(COMMAND "${TOOLS}" scene dump "${scene}" OUTPUT_FILE "${base}.txt"
                    RESULT_VARIABLE code ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        fail("scene dump of ${scene} failed (${code})" "" "${err}")
    endif()
    execute_process(COMMAND "${TOOLS}" scene build "${base}.txt" "${base}.scene" RESULT_VARIABLE code ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        fail("scene build of the dump of ${scene} failed (${code})" "" "${err}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${base}.scene" "${scene}" RESULT_VARIABLE differs)
    if(NOT differs EQUAL 0)
        fail("${scene}: dump + build did not give back the same bytes" "" "")
    endif()
    execute_process(COMMAND "${TOOLS}" scene dump "${base}.scene" OUTPUT_FILE "${base}.again.txt" RESULT_VARIABLE code)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${base}.again.txt" "${base}.txt" RESULT_VARIABLE differs)
    if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
        fail("${scene}: the text view is not stable" "" "")
    endif()
    execute_process(COMMAND "${TOOLS}" scene upgrade "${scene}" "${base}.upgraded.scene" RESULT_VARIABLE code ERROR_VARIABLE err)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${base}.upgraded.scene" "${scene}" RESULT_VARIABLE differs)
    if(NOT code EQUAL 0 OR NOT differs EQUAL 0)
        fail("${scene}: scene upgrade changed a current-version file (${code})" "" "${err}")
    endif()
endforeach()

# 3. replay verification on 1 and 4 threads, and a drifted project is refused ----------
foreach(jobs 1 4)
    execute_process(
        COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/run.replay" --jobs ${jobs}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT "${out}${err}" MATCHES "${TICK} ticks replayed on ${jobs} thread")
        fail("--replay-verify on ${jobs} job(s) failed (${code})" "${out}" "${err}")
    endif()
    string(REGEX MATCH "final state hash ([0-9a-f]+)" m "${out}${err}")
    if(NOT CMAKE_MATCH_1 STREQUAL hash)
        fail("--replay-verify ended at ${CMAKE_MATCH_1}, the run at ${hash}" "${out}" "${err}")
    endif()
endforeach()

file(GLOB scripts RELATIVE "${PROJECT}" "${PROJECT}/scripts/*.plm")
list(SORT scripts)
list(LENGTH scripts script_count)
if(script_count GREATER 0)
    list(GET scripts 0 script)
    file(COPY "${PROJECT}/" DESTINATION "${OUT}/drifted")
    file(APPEND "${OUT}/drifted/${script}" "\nclass SweepDrift {\n    static var amount = 1\n}\n")
    execute_process(
        COMMAND "${EDITOR}" --project "${OUT}/drifted" --replay-verify "${OUT}/run.replay" --jobs 1
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "mismatch at tick 1 of ${TICK}")
        fail("--replay-verify accepted a drifted project (${code})" "${out}" "${err}")
    endif()
endif()

# 4. hot reload ----------------------------------------------------------------------
# A scripted edit + forced reload mid-play (DESIGN.md 5.13). A project whose reload
# touches no coroutine and allocates no emitter (RELOAD_EXACT) must keep the run
# bit for bit: the reference is the runtime's run without --press. Others must keep
# playing: the tick count is reached and the game is not panicked. A recording made
# across the reload is a loadable replay (it plays on with the scripts as they are
# then, game_session.cpp).
if(script_count GREATER 0)
    run_game(idle_hash "${RUNTIME}" idle --game "${PROJECT}" --jobs 1)
    file(COPY "${PROJECT}/" DESTINATION "${OUT}/reload")
    math(EXPR first "${TICK} / 3")
    math(EXPR rest "${TICK} - ${first}")
    set(expect_hash "")
    if(RELOAD_EXACT)
        set(expect_hash "expect-hash ${idle_hash}\n")
    endif()
    file(WRITE "${OUT}/reload.cmds"
        "play\n"
        "step ${first}\n"
        "append-file ${script} // edited while playing\n"
        "reload\n"
        "step ${rest}\n"
        "expect-tick ${TICK}\n"
        "expect-state playing\n"
        "${expect_hash}"
        "stop\n"
        "play-record\n"
        "step ${first}\n"
        "append-file ${script} // edited again\n"
        "reload\n"
        "step ${rest}\n"
        "expect-state playing\n"
        "${expect_hash}"
        "record stop\n"
        "record save reload.replay\n"
        "stop\n"
        "replay load reload.replay\n"
        "replay seek ${TICK}\n"
        "stop\n")
    execute_process(
        COMMAND "${EDITOR}" --project "${OUT}/reload" --no-audio --script-commands "${OUT}/reload.cmds"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR "${out}${err}" MATCHES "\\[error\\]|panic")
        fail("a hot reload while playing failed (${code})" "${out}" "${err}")
    endif()
    if(RELOAD_EXACT)
        # Nothing restarted, so the replay recorded across the reload verifies against the unedited project.
        execute_process(
            COMMAND "${EDITOR}" --project "${PROJECT}" --replay-verify "${OUT}/reload/reload.replay" --jobs 1
            RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT code EQUAL 0)
            fail("the replay recorded across a hot reload does not verify (${code})" "${out}" "${err}")
        endif()
    endif()
else()
    message(STATUS "${NAME}: no scripts, nothing to hot reload")
endif()

# 5. exports --------------------------------------------------------------------------
set(variants bytecode aot)
if(DEFINED PLAYER_CACHE AND NOT PLAYER_CACHE STREQUAL "")
    list(APPEND variants stripped)
endif()
foreach(variant ${variants})
    file(COPY "${PROJECT}/" DESTINATION "${OUT}/src_${variant}/${NAME}")
    if(variant STREQUAL "bytecode")
        set(export_args --engine full --strip)
    elseif(variant STREQUAL "aot")
        set(export_args --engine full --aot --strip)
    else()
        set(export_args --engine stripped --strip --player-cache "${PLAYER_CACHE}")
    endif()
    execute_process(
        COMMAND "${TOOLS}" export "${OUT}/src_${variant}/${NAME}" --platform ${PLATFORM} ${export_args} -o "${OUT}/dist_${variant}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        fail("export (${variant}) failed (${code})" "${out}" "${err}")
    endif()
    file(REMOVE_RECURSE "${OUT}/src_${variant}") # only dist/ ships
    file(GLOB entries "${OUT}/dist_${variant}/*${EXE_SUFFIX}")
    set(launcher "")
    foreach(e ${entries})
        get_filename_component(leaf "${e}" NAME)
        if(NOT IS_DIRECTORY "${e}" AND NOT leaf MATCHES "\\.(pak|txt|md|so|dll)$")
            set(launcher "${e}")
        endif()
    endforeach()
    if(launcher STREQUAL "")
        fail("export (${variant}) has no launcher in ${OUT}/dist_${variant}" "${out}" "${err}")
    endif()
    file(MAKE_DIRECTORY "${OUT}/cwd_${variant}" "${OUT}/user_${variant}")
    set(ENV{CUERVO_USER_DIR} "${OUT}/user_${variant}")
    execute_process(
        COMMAND "${launcher}" ${press_args} --backend gl --no-audio --screenshot "${OUT}/${variant}.ppm"
        WORKING_DIRECTORY "${OUT}/cwd_${variant}"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(log "${out}${err}")
    if(NOT code EQUAL 0 OR NOT log MATCHES "Pack '")
        fail("the exported game (${variant}) failed or did not run from its pack (${code})" "${out}" "${err}")
    endif()
    if(log MATCHES "\\[error\\]|panic|examples/")
        fail("errors (or project paths) while running the exported game (${variant})" "${out}" "${err}")
    endif()
    # A stripped player's hash covers fewer preludes (DESIGN.md 4.11): its run is checked by the export itself.
    if(NOT variant STREQUAL "stripped" AND NOT log MATCHES "at tick ${TICK} \\(state hash ${hash}\\)")
        fail("the exported game (${variant}) did not reach the dev hash ${hash} at tick ${TICK}" "${out}" "${err}")
    endif()
    # The frame itself is not compared byte for byte: the GPU may differ by one colour step in a pixel
    # between runs of the very same build (bench, seen on NVIDIA); the state hash is the exact check.
    file(SIZE "${OUT}/${variant}.ppm" exported_size)
    file(SIZE "${OUT}/dev.ppm" dev_size)
    if(NOT exported_size EQUAL dev_size)
        fail("the exported game's (${variant}) screenshot has another size than the dev run's" "" "")
    endif()
endforeach()
message(STATUS "${NAME}: dev ${hash} on 1/4 jobs, ${scene_count} scene(s) round-trip, replay verified, hot reload kept, exports [${variants}] agree")
