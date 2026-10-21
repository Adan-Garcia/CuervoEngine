# The language library must never depend on the engine (DESIGN.md 3).
add_library(pluma STATIC
    aot.cpp
    ast.cpp
    acyclic.cpp
    ast_dump.cpp
    checker.cpp
    collections.cpp
    compiler.cpp
    diagnostic.cpp
    format.cpp
    heap.cpp
    lexer.cpp
    lsp.cpp
    dap.cpp
    parser.cpp
    printer.cpp
    profiler.cpp
    reload.cpp
    sema.cpp
    session.cpp
    source.cpp
    stdlib.cpp
    value.cpp
    vm.cpp
    vm_lib.cpp
)
# The Pluma-source standard library (pluma/std/*.plm) is embedded as a raw
# string literal; editing it re-runs this step.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/std/math.plm" _pluma_math_text)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/pluma_std_math.inc"
     CONTENT "R\"__plm__(${_pluma_math_text})__plm__\"" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/std/math.plm")
target_include_directories(pluma PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pluma PUBLIC project::flags ${CMAKE_DL_LIBS})
target_compile_definitions(pluma PRIVATE PLUMA_DEFAULT_CC="${CMAKE_C_COMPILER}")

add_executable(plm repl/main.cpp)
target_link_libraries(plm PRIVATE pluma)
