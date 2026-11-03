# The editor (DESIGN.md 4.13), desktop only. editor_core is everything that
# doesn't need a window or a UI library (scene model, undo stack, class registry,
# project/preview, command scripts), so tests/editor links it alone.

add_library(editor_core STATIC
    scene_model.cpp
    commands.cpp
    class_registry.cpp
    class_catalog.cpp
    project_env.cpp
    editor_controller.cpp
    editor_document.cpp
    document_selection.cpp
    actions.cpp
    layouts.cpp
    projects.cpp
    messages.cpp
    icons.cpp
    rml_widgets.cpp
    tree_view.cpp
    inspector_model.cpp
    command_script.cpp
    cli_run.cpp
    console_log.cpp
    view_camera.cpp
    scene_painter.cpp
    scene_input.cpp
    play_controller.cpp
    scene_preview.cpp
    asset_library.cpp
    input_map_doc.cpp
    live_tree.cpp
    settings.cpp
    theme.cpp
    anim_edit.cpp
    sprite_tools.cpp
    sprite_view.cpp
    timeline.cpp
    particle_preview.cpp
    file_import.cpp
    text_buffer.cpp
    script_debug.cpp
    script_assist.cpp
    script_find.cpp
    script_syntax.cpp
    script_workspace.cpp
)
target_link_libraries(editor_core PUBLIC cuervo PRIVATE SDL3::SDL3)

add_executable(editor
    main.cpp
    rml/rml_rhi.cpp
    rml/rml_theme.cpp
    rml/rml_ui.cpp
    rml/rml_shell.cpp
    rml/rml_inspector.cpp
    rml/rml_dialogs.cpp
    rml/rml_drawer.cpp
    rml/rml_sprite.cpp
    rml/rml_particles.cpp
    rml/rml_timeline.cpp
    rml/rml_profiler.cpp
    rml/rml_panels.cpp
    rml/rml_entry.cpp
    rml/rml_welcome.cpp
    rml/rml_pick.cpp
    rml/rml_assets.cpp
    rml/script/code_view.cpp
    rml/script/script_panel.cpp
    rml/script/script_glue.cpp
    ${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.cpp
    splash.cpp
)
# The UI shaders (both editors draw through them) go through the engine's shader pipeline (cmake/Shaders.cmake).
cuervo_add_shaders(editor
    shaders/editor_ui.vert
    shaders/editor_ui.frag
)
target_link_libraries(editor PRIVATE editor_core SDL3::SDL3 rmlui_core)
# The deprecated ImGui editor (`--imgui`, DESIGN.md 4.13): compiled in only with CUERVO_EDITOR_IMGUI.
# Deleting it later means deleting this block, the option and these files.
if(CUERVO_EDITOR_IMGUI)
    target_sources(editor PRIVATE
        app.cpp
        imgui_rhi.cpp
        imgui_theme.cpp
        app_ui.cpp
        ui.cpp
        launcher.cpp
        license_view.cpp
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
        history_panel.cpp
        animation_panel.cpp
        sprites_panel.cpp
        particles_panel.cpp
    )
    target_link_libraries(editor PRIVATE imgui)
    target_compile_definitions(editor PRIVATE CUERVO_EDITOR_IMGUI=1)
endif()
# The RmlUi spike (--rml-ui): its SDL3 platform file is compiled from the pinned tarball, unedited.
target_include_directories(editor SYSTEM PRIVATE "${rmlui_SOURCE_DIR}/Backends")
target_compile_definitions(editor PRIVATE RMLUI_SDL_VERSION_MAJOR=3 "CUERVO_RML_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/rml\"" "CUERVO_EDITOR_VERSION=\"${PROJECT_VERSION}\"" "CUERVO_EXAMPLES_DIR=\"${PROJECT_SOURCE_DIR}/examples\"")
# Third-party file, compiled unedited and without our warning level (-w on GCC/Clang, /w on MSVC).
if(MSVC)
    set_source_files_properties("${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.cpp" PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties("${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.cpp" PROPERTIES COMPILE_OPTIONS "-w")
endif()
set_target_properties(editor PROPERTIES OUTPUT_NAME cuervo-editor)
# Windows: a GUI program (no console window); started from a terminal it logs there (docs/WINDOWS.md 5).
cuervo_windows_app(editor GUI)
target_compile_definitions(editor PRIVATE
    CUERVO_DEFAULT_FONT="${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf"
    # The brand (DESIGN.md 4.16): synced SVGs for the splash, and the fonts
    # fetched in cmake/Dependencies.cmake (development-only, like the editor).
    CUERVO_BRAND_DIR="${PROJECT_SOURCE_DIR}/assets/brand"
    CUERVO_FONT_DISPLAY="${CUERVO_FONT_DISPLAY}"
    CUERVO_FONT_UI="${CUERVO_FONT_UI}"
    CUERVO_FONT_UI_BOLD="${CUERVO_FONT_UI_BOLD}"
    CUERVO_FONT_MONO="${CUERVO_FONT_MONO}")
