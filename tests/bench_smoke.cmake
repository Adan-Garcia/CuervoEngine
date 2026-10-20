# ctest bench_smoke (milestone 8.6): tools/bench.py end to end on GL offscreen with tiny, short runs.
# Asserts only that the tool runs, the JSON validates, the state hash is the same for 1 and N jobs, a
# baseline compares equal to itself, and the capacity search records monotone-consistent probes. It never
# asserts a timing, so it cannot flake. Inputs: PYTHON, BENCH, BUILD, OUT.
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(ENV{SDL_VIDEO_DRIVER} offscreen)
set(_json ${OUT}/baseline.json)
function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed (${_rc}):\n${_out}\n${_err}")
    endif()
    set(RUN_OUT "${_out}" PARENT_SCOPE)
endfunction()
run(${PYTHON} -I ${BENCH} record --build-dir ${BUILD} --out ${_json} --workloads 1000 --runs 1 --warmup 1
    --duration 1 --no-export --machine-id smoke)
run(${PYTHON} -I ${BENCH} check ${_json})
run(${PYTHON} -I ${BENCH} compare ${_json} ${_json} --scale 100)
if(NOT RUN_OUT MATCHES "RESULT: ok")
    message(FATAL_ERROR "compare did not report ok:\n${RUN_OUT}")
endif()
run(${PYTHON} -I ${BENCH} capacity --build-dir ${BUILD} --out ${_json} --machine-id smoke --scenes dense sparse
    --runs 1 --warmup 1 --duration 1 --min 1000 --max 4000 --resolution 0.5)
run(${PYTHON} -I ${BENCH} check ${_json})
file(READ ${_json} _text)
foreach(_scene dense sparse)
    if(NOT _text MATCHES "\"${_scene}\"")
        message(FATAL_ERROR "no capacity record for ${_scene}")
    endif()
endforeach()
