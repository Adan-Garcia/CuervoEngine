# ctest trace_shmup (milestone 11.4): `cuervo --game examples/shmup --trace` writes a Chrome trace; parse it
# with CMake's JSON support and check it shows per-system tick cost (zone spans with a duration), the frame
# loop, per-frame counters and thread metadata. Never asserts a timing value beyond "positive".
# Inputs: RUNTIME, PROJECT, OUT.
file(REMOVE ${OUT})
set(ENV{SDL_VIDEO_DRIVER} offscreen)
execute_process(COMMAND ${RUNTIME} --game ${PROJECT} --backend gl --no-audio --quit-after 1 --trace ${OUT}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "runtime failed (${_rc}):\n${_out}\n${_err}")
endif()
if(NOT EXISTS ${OUT})
    message(FATAL_ERROR "no trace written:\n${_out}\n${_err}")
endif()
file(READ ${OUT} _json)
string(JSON _n LENGTH "${_json}" traceEvents)
if(_n LESS 10)
    message(FATAL_ERROR "trace has only ${_n} events")
endif()
# string(JSON ... GET) rescans the document on every call (quadratic for ~10k events), so the event
# checks use regular expressions over the writer's fixed key order; the LENGTH call above is the parse.
foreach(_want frame simulation scripts update render)
    if(NOT _json MATCHES "\"name\":\"${_want}\",\"cat\":\"zone\",\"ph\":\"X\",\"ts\":[0-9.]+,\"dur\":(0\\.0*[1-9]|[1-9])")
        message(FATAL_ERROR "trace has no '${_want}' zone span with a positive duration")
    endif()
endforeach()
foreach(_want draw_calls heap_live)
    if(NOT _json MATCHES "\"name\":\"${_want}\",\"cat\":\"counter\",\"ph\":\"C\"")
        message(FATAL_ERROR "trace has no '${_want}' counter")
    endif()
endforeach()
if(NOT _json MATCHES "\"thread_name\"")
    message(FATAL_ERROR "trace has no thread_name metadata")
endif()
message(STATUS "trace ok: ${_n} events")
