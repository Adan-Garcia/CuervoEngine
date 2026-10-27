set(RUNTIME_SOURCES main.cpp game_cli.cpp)
if(NOT EMSCRIPTEN)
    list(APPEND RUNTIME_SOURCES dap_cli.cpp) # the debug adapter (stdio): desktop only
endif()
if(CUERVO_FEATURE_DEV)
    list(APPEND RUNTIME_SOURCES svg_viewer.cpp vector_demo.cpp particles_demo.cpp)
endif()
add_executable(runtime ${RUNTIME_SOURCES})
target_link_libraries(runtime PRIVATE cuervo SDL3::SDL3)
set_target_properties(runtime PROPERTIES OUTPUT_NAME cuervo)
# Development defaults; --font / --svg override them.
target_compile_definitions(runtime PRIVATE
    CUERVO_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf"
    CUERVO_DEMO_SVG="${PROJECT_SOURCE_DIR}/assets/demo/robot.svg")
cuervo_windows_app(runtime)

# Windows: the same program as a GUI-subsystem player, `cuervo-player.exe`, so an
# exported game opens no console window (docs/WINDOWS.md 5). `cuervo.exe` stays
# a console program for development; `export --platform windows` ships this one.
if(WIN32)
    add_executable(player ${RUNTIME_SOURCES})
    target_link_libraries(player PRIVATE cuervo SDL3::SDL3)
    set_target_properties(player PROPERTIES OUTPUT_NAME cuervo-player)
    target_compile_definitions(player PRIVATE
        CUERVO_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf"
        CUERVO_DEMO_SVG="${PROJECT_SOURCE_DIR}/assets/demo/robot.svg")
    cuervo_windows_app(player GUI)
endif()

# Web: index.html + .js + .wasm + .data with the preloaded project (cmake/Web.cmake).
if(EMSCRIPTEN)
    cuervo_web_app(runtime)
endif()
