# The editor (DESIGN.md 4.13), desktop only. editor_core is everything that
# doesn't need a window or ImGui (scene model, undo stack, class registry,
# project/preview, command scripts), so tests/editor links it alone.

add_library(editor_core STATIC
    scene_model.cpp
    commands.cpp
    class_registry.cpp
    project_env.cpp
    editor_document.cpp
    document_selection.cpp
    actions.cpp
    layouts.cpp
    projects.cpp
    messages.cpp
    icons.cpp
    command_script.cpp
    console_log.cpp
    view_camera.cpp
    play_controller.cpp
    asset_library.cpp
    input_map_doc.cpp
    live_tree.cpp
)
target_link_libraries(editor_core PUBLIC cuervo)

add_executable(editor
    main.cpp
    app.cpp
    imgui_rhi.cpp
    scene_tree_panel.cpp
    scene_view_panel.cpp
    inspector_panel.cpp
    console_panel.cpp
    asset_panel.cpp
    input_map_panel.cpp
    debugger_panel.cpp
    replay_panel.cpp
    tier_panel.cpp
    profiler_panel.cpp
    theme.cpp
    splash.cpp
    app_ui.cpp
    ui.cpp
    history_panel.cpp
    license_view.cpp
    launcher.cpp
)
# The ImGui shaders go through the engine's shader pipeline (cmake/Shaders.cmake).
cuervo_add_shaders(editor
    shaders/imgui.vert
    shaders/imgui.frag
)
target_link_libraries(editor PRIVATE editor_core imgui SDL3::SDL3)
set_target_properties(editor PROPERTIES OUTPUT_NAME cuervo-editor)
target_compile_definitions(editor PRIVATE
    CUERVO_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf"
    # The brand (DESIGN.md 4.16): synced SVGs for the splash, and the fonts
    # fetched in cmake/Dependencies.cmake (development-only, like the editor).
    CUERVO_BRAND_DIR="${PROJECT_SOURCE_DIR}/assets/brand"
    CUERVO_FONT_DISPLAY="${CUERVO_FONT_DISPLAY}"
    CUERVO_FONT_UI="${CUERVO_FONT_UI}"
    CUERVO_FONT_MONO="${CUERVO_FONT_MONO}")
