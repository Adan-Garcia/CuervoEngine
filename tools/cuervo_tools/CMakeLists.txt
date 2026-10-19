# `cuervo-tools`: command-line tools for project files (DESIGN.md 3 "tools/").
# The release pipeline (pack/export, docs/EXPORT.md) is a library so the tests
# can drive it too.
add_library(cuervo_export STATIC export.cpp player.cpp)
target_link_libraries(cuervo_export PUBLIC cuervo)

# The API reference generator (`cuervo-tools docs`, docs/API.md) only needs
# the Pluma parser; a library so tests can feed it a synthetic prelude.
add_library(cuervo_api_docs STATIC api_docs.cpp)
target_link_libraries(cuervo_api_docs PUBLIC pluma)

add_executable(cuervo_tools main.cpp)
set_target_properties(cuervo_tools PROPERTIES OUTPUT_NAME cuervo-tools)
target_link_libraries(cuervo_tools PRIVATE cuervo_export cuervo_api_docs)
# Export defaults: the engine font, this build tree's player (the native
# runtime target) and the pack-loading web runtime (the web-pack preset).
# Stripped players (milestone 7.5, docs/EXPORT.md "Stripped players") are
# configured from these sources with this configure's cmake, generator and
# compilers, reusing this tree's FetchContent checkouts.
target_compile_definitions(cuervo_tools PRIVATE
    CUERVO_TOOLS_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf"
    CUERVO_TOOLS_RUNTIME="$<TARGET_FILE:runtime>"
    CUERVO_TOOLS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    CUERVO_TOOLS_WEB_RUNTIME="${PROJECT_SOURCE_DIR}/build/web-pack/runtime"
    CUERVO_TOOLS_ENGINE_VERSION="${PROJECT_VERSION}"
    CUERVO_TOOLS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    CUERVO_TOOLS_CMAKE="${CMAKE_COMMAND}"
    CUERVO_TOOLS_GENERATOR="${CMAKE_GENERATOR}"
    CUERVO_TOOLS_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
    CUERVO_TOOLS_C_COMPILER="${CMAKE_C_COMPILER}"
    CUERVO_TOOLS_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    CUERVO_TOOLS_DEPS_DIR="${FETCHCONTENT_BASE_DIR}")
