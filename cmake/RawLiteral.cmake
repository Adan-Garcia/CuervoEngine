# cuervo_raw_literal(<out var> <delimiter> <text>)
#
# Sets <out var> to C++ source for <text> as adjacent raw string literals of
# at most ~8 KB each, split after line ends. MSVC rejects a single literal
# over ~16 KB (C2026) but joins adjacent ones up to 64 KB, and GCC/Clang
# accept the same form, so every embedded text goes through here.
function(cuervo_raw_literal out delim text)
    set(_pieces "")
    while(TRUE)
        string(LENGTH "${text}" _len)
        if(_len LESS_EQUAL 8000)
            break()
        endif()
        string(SUBSTRING "${text}" 8000 -1 _rest)
        string(FIND "${_rest}" "\n" _nl)
        if(_nl EQUAL -1)
            break()
        endif()
        math(EXPR _cut "8000 + ${_nl} + 1")
        string(SUBSTRING "${text}" 0 ${_cut} _head)
        string(APPEND _pieces "R\"${delim}(${_head})${delim}\"\n")
        string(SUBSTRING "${text}" ${_cut} -1 text)
    endwhile()
    string(APPEND _pieces "R\"${delim}(${text})${delim}\"")
    set(${out} "${_pieces}" PARENT_SCOPE)
endfunction()
