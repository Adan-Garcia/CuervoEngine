# Golden screenshots of the RmlUi editor on offscreen GL (see tests/golden/README.md):
# each case starts `cuervo-editor --rml-ui` on a copy of examples/editor_level with fixed size, scale and
# theme, optionally posts input with --ui-events, captures the window and compares a crop with
# tests/golden/<name>.qoi through golden_diff (per-channel tolerance, small fraction of pixels).
# Regenerate after an intended visual change:  CUERVO_UPDATE_GOLDENS=1 ctest -R editor_golden
# Invoked by ctest with -DEDITOR=... -DDIFF=... -DPROJECT=... -DGOLDEN_DIR=... -DOUT=...
if(CMAKE_HOST_WIN32)
    message(STATUS "SKIPPED: the offscreen SDL driver is not used on Windows")
    return()
endif()
set(ENV{SDL_VIDEO_DRIVER} offscreen)
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/user")
file(COPY "${PROJECT}/project.toml" "${PROJECT}/scripts" "${PROJECT}/scenes" DESTINATION "${OUT}/project")
set(ENV{CUERVO_USER_DIR} "${OUT}/user")
set(update FALSE)
if("$ENV{CUERVO_UPDATE_GOLDENS}" STREQUAL "1")
    set(update TRUE)
    file(MAKE_DIRECTORY "${GOLDEN_DIR}")
endif()
set(golden_project "${OUT}/project") # a case may switch to another copy by setting this before calling golden()

# name, crop x,y,w,h, then the editor arguments
function(golden name crop)
    execute_process(COMMAND "${EDITOR}" --rml-ui --project "${golden_project}" --backend gl --no-audio --no-splash --size 800x520 --theme dark --ui-scale 1.0 ${ARGN} --screenshot "${OUT}/${name}.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0 OR NOT EXISTS "${OUT}/${name}.ppm")
        message(FATAL_ERROR "${name}: the editor exited ${code}\n${out}\n${err}")
    endif()
    set(mode compare)
    if(update)
        set(mode update)
    endif()
    execute_process(COMMAND "${DIFF}" ${mode} "${OUT}/${name}.ppm" "${GOLDEN_DIR}/${name}.qoi" "${crop}" --diff "${OUT}/${name}.diff.ppm"
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message(STATUS "${out}")
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "${name}: differs from tests/golden/${name}.qoi\n${out}${err}\nactual: ${OUT}/${name}.ppm  diff: ${OUT}/${name}.diff.ppm\nIf the change is intended: CUERVO_UPDATE_GOLDENS=1 ctest -R editor_golden")
    endif()
endfunction()

golden(shell "0,0,800,520")
golden(inspector "540,40,260,440" --select /ship)
golden(tree_drag "0,40,330,300" --select /ship
       --ui-events "move 120,185|down 120,185|move 120,200|move 125,230|move 125,250")
# crop: the New project card only (the recent list shows this machine's paths, the examples list grows)
golden(welcome "444,120,356,170" --screen welcome)
golden(modal "0,0,800,520" --ui-events "key ctrl+o")

# An SVG on a Sprite2D: the Sprites tab rasterizes it over the transparency checkerboard.
file(COPY "${OUT}/project/" DESTINATION "${OUT}/sprite_project")
file(MAKE_DIRECTORY "${OUT}/sprite_project/art")
file(WRITE "${OUT}/sprite_project/art/box.svg" "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='48' viewBox='0 0 64 48'><circle cx='32' cy='24' r='20' fill='#e06030'/><rect x='4' y='4' width='16' height='10' fill='#3090e0' opacity='0.5'/></svg>\n")
file(WRITE "${OUT}/sprite.cmds" "open scenes/level.scene\nadd / Sprite2D vec\nset /vec asset \"art/box.svg\"\nsave\n")
execute_process(COMMAND "${EDITOR}" --rml-ui --project "${OUT}/sprite_project" --no-audio --script-commands "${OUT}/sprite.cmds"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "preparing the sprite project failed (${code}):\n${out}\n${err}")
endif()
set(golden_project "${OUT}/sprite_project")
golden(sprite "328,326,46,104" --select /vec --tabs Sprites)
