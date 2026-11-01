# Runs the real LSP and DAP programs with files as stdin/stdout and checks the
# bytes of the Content-Length header. On Windows a stdout in text mode turned
# "\r\n" into "\r\r\n" and clients never found the end of the header; the
# in-process framing tests (lsp_tests.cpp) can't see that.
# Inputs: TOOLS, RUNTIME, OUT.

file(MAKE_DIRECTORY "${OUT}")

# file(WRITE) writes in text mode on Windows and turns each "\n" into "\r\n", so a
# literal "\r\n" there would reach the program as "\r\r\n".
if(WIN32)
    set(crlf "\n")
else()
    set(crlf "\r\n")
endif()

function(check_framing name exe args body)
    string(LENGTH "${body}" len)
    file(WRITE "${OUT}/${name}.in" "Content-Length: ${len}${crlf}${crlf}${body}")
    execute_process(COMMAND "${exe}" ${args}
        INPUT_FILE "${OUT}/${name}.in" OUTPUT_FILE "${OUT}/${name}.out"
        RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 60)
    file(READ "${OUT}/${name}.out" hex HEX)
    string(FIND "${hex}" "0d0d0a" bad)
    if(NOT bad EQUAL -1)
        message(FATAL_ERROR "${name}: header contains \\r\\r\\n (stdout in text mode)")
    endif()
    # "Content-Length:" then, somewhere after the number, exactly CRLF CRLF.
    if(NOT hex MATCHES "^436f6e74656e742d4c656e6774683a[0-9a-f]*0d0a0d0a")
        message(FATAL_ERROR "${name}: no 'Content-Length: N\\r\\n\\r\\n' header in the output (rc=${rc}, ${exe} ${args})\nstderr: ${err}\noutput hex: ${hex}")
    endif()
endfunction()

check_framing(lsp "${TOOLS}" "lsp"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}")
check_framing(dap "${RUNTIME}" "--dap"
    "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"adapterID\":\"pluma\"}}")
