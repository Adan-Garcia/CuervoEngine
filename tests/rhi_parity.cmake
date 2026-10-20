# RHI parity test (DESIGN.md 6, Phase 5): renders every RHI feature scene
# (tools/rhi_parity) and a shmup frame on each backend through the
# frame-capture path and compares the images.
#
#   GL 3.3 core vs GLES 3.0 (both headless, SDL_VIDEO_DRIVER=offscreen): always.
#   Vulkan vs GL: only with a display (WAYLAND_DISPLAY or DISPLAY set; NVIDIA
#   has no VK_EXT_headless_surface). Set CUERVO_PARITY_VULKAN=0 to skip it.
#
# cmake -DRHI_PARITY=<exe> -DRUNTIME=<exe> -DSHMUP=<dir> -DOUT=<dir> -P rhi_parity.cmake

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    message("${out}${err}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "failed (${result}): ${ARGN}")
    endif()
endfunction()

if(CMAKE_HOST_WIN32)
    set(offscreen ${CMAKE_COMMAND} -E env CUERVO_AUDIO=null)
else()
    set(offscreen ${CMAKE_COMMAND} -E env SDL_VIDEO_DRIVER=offscreen CUERVO_AUDIO=null)
endif()

run(${offscreen} "${RHI_PARITY}" render --backend gl --out "${OUT}/gl")
run(${offscreen} "${RHI_PARITY}" render --backend gl --gl-es --out "${OUT}/gles")
# Same GL driver, two shading-language paths: must be identical.
run("${RHI_PARITY}" compare "${OUT}/gl" "${OUT}/gles" --tolerance 0)

# Many frames on one device: rotating stream versions and the frame fence ring
# (first and last frame identical; at most the bound queued).
run(${offscreen} "${RHI_PARITY}" frames --backend gl --out "${OUT}/frames_gl")
run(${offscreen} "${RHI_PARITY}" frames --backend gl --gl-es --out "${OUT}/frames_gles")

set(shmup_args --game "${SHMUP}" --no-audio --no-hot-reload --screenshot)
run(${offscreen} "${RUNTIME}" ${shmup_args} "${OUT}/shmup_gl.ppm" --backend gl)
run(${offscreen} "${RUNTIME}" ${shmup_args} "${OUT}/shmup_gles.ppm" --backend gl --gl-es)
run("${RHI_PARITY}" compare "${OUT}/shmup_gl.ppm" "${OUT}/shmup_gles.ppm" --tolerance 0)

set(has_display FALSE)
if(CMAKE_HOST_WIN32 OR NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "" OR NOT "$ENV{DISPLAY}" STREQUAL "")
    set(has_display TRUE)
endif()

if(has_display AND NOT "$ENV{CUERVO_PARITY_VULKAN}" STREQUAL "0")
    run("${RHI_PARITY}" render --backend vulkan --out "${OUT}/vulkan")
    run("${RHI_PARITY}" compare "${OUT}/gl" "${OUT}/vulkan")
    # Full tier: base-vertex draws must match their reference (checked by render).
    run("${RHI_PARITY}" frames --backend vulkan --out "${OUT}/frames_vulkan")
    run("${RHI_PARITY}" render --backend vulkan --tier full --out "${OUT}/vulkan_full")
    run("${RUNTIME}" ${shmup_args} "${OUT}/shmup_vulkan.ppm" --backend vulkan)
    run("${RHI_PARITY}" compare "${OUT}/shmup_gl.ppm" "${OUT}/shmup_vulkan.ppm")
else()
    message("Vulkan parity skipped: no display (WAYLAND_DISPLAY/DISPLAY unset) or CUERVO_PARITY_VULKAN=0")
endif()
