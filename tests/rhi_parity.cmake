# RHI parity test (DESIGN.md 6, Phase 5): renders every RHI feature scene
# (tools/rhi_parity) and a shmup frame on each backend through the
# frame-capture path and compares the images. With -DLIT=<examples/lantern_cave>
# it also compares a frame of that 2D-lit game (milestone 9.7: lights and
# shadows render identically on Vulkan and GL).
#
#   GL 3.3 core vs GLES 3.0 (both headless, SDL_VIDEO_DRIVER=offscreen): always.
#   Vulkan vs GL: only with a display (WAYLAND_DISPLAY or DISPLAY set; NVIDIA
#   has no VK_EXT_headless_surface). Set CUERVO_PARITY_VULKAN=0 to skip it.
#   WebGPU vs GL (milestone 10.1, -DWEBGPU=ON): headless, the device renders
#   offscreen without a window surface; same edge tolerance as Vulkan. With a
#   display, the WebGPU scenes must also equal Vulkan's exactly (10.2). With
#   -DBENCH=<examples/bench>, also its 1k workload at tick 600, whose stream
#   buffer grows mid-frame (10.4: a stale vertex binding once broke it).
#
# cmake -DRHI_PARITY=<exe> -DRUNTIME=<exe> -DSHMUP=<dir> [-DLIT=<dir>] [-DBENCH=<dir>] [-DWEBGPU=ON] -DOUT=<dir> -P rhi_parity.cmake

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

if(DEFINED LIT AND NOT LIT STREQUAL "")
    set(lit_args --game "${LIT}" --no-audio --no-hot-reload
        --press "10-114:move_right,60:jump,124:jump,138-240:move_right" --screenshot)
    run(${offscreen} "${RUNTIME}" ${lit_args} "${OUT}/lit_gl.ppm" --backend gl)
    run(${offscreen} "${RUNTIME}" ${lit_args} "${OUT}/lit_gles.ppm" --backend gl --gl-es)
    run("${RHI_PARITY}" compare "${OUT}/lit_gl.ppm" "${OUT}/lit_gles.ppm" --tolerance 0)
endif()

if(WEBGPU)
    run(${offscreen} "${RHI_PARITY}" render --backend webgpu --out "${OUT}/webgpu")
    run("${RHI_PARITY}" compare "${OUT}/gl" "${OUT}/webgpu")
    run(${offscreen} "${RHI_PARITY}" frames --backend webgpu --out "${OUT}/frames_webgpu")
    run(${offscreen} "${RUNTIME}" ${shmup_args} "${OUT}/shmup_webgpu.ppm" --backend webgpu)
    run("${RHI_PARITY}" compare "${OUT}/shmup_gl.ppm" "${OUT}/shmup_webgpu.ppm")
    if(DEFINED LIT AND NOT LIT STREQUAL "")
        run(${offscreen} "${RUNTIME}" ${lit_args} "${OUT}/lit_webgpu.ppm" --backend webgpu)
        run("${RHI_PARITY}" compare "${OUT}/lit_gl.ppm" "${OUT}/lit_webgpu.ppm")
    endif()
    if(DEFINED BENCH AND NOT BENCH STREQUAL "")
        set(bench_args --game "${BENCH}" --no-audio --no-hot-reload --press "5:more,15:more"
            --screenshot-tick 600 --screenshot)
        run(${offscreen} "${RUNTIME}" ${bench_args} "${OUT}/bench_gl.ppm" --backend gl)
        run(${offscreen} "${RUNTIME}" ${bench_args} "${OUT}/bench_webgpu.ppm" --backend webgpu)
        run("${RHI_PARITY}" compare "${OUT}/bench_gl.ppm" "${OUT}/bench_webgpu.ppm")
    endif()
endif()

set(has_display FALSE)
if(CMAKE_HOST_WIN32 OR NOT "$ENV{WAYLAND_DISPLAY}" STREQUAL "" OR NOT "$ENV{DISPLAY}" STREQUAL "")
    set(has_display TRUE)
endif()

if(has_display AND NOT "$ENV{CUERVO_PARITY_VULKAN}" STREQUAL "0")
    run("${RHI_PARITY}" render --backend vulkan --out "${OUT}/vulkan")
    run("${RHI_PARITY}" compare "${OUT}/gl" "${OUT}/vulkan")
    if(WEBGPU)
        # wgpu-native runs on Vulkan here: same rasterization rules, same pixels.
        run("${RHI_PARITY}" compare "${OUT}/vulkan" "${OUT}/webgpu" --tolerance 0)
    endif()
    # Full tier: base-vertex draws must match their reference (checked by render).
    run("${RHI_PARITY}" frames --backend vulkan --out "${OUT}/frames_vulkan")
    run("${RHI_PARITY}" render --backend vulkan --tier full --out "${OUT}/vulkan_full")
    run("${RUNTIME}" ${shmup_args} "${OUT}/shmup_vulkan.ppm" --backend vulkan)
    run("${RHI_PARITY}" compare "${OUT}/shmup_gl.ppm" "${OUT}/shmup_vulkan.ppm")
    if(DEFINED LIT AND NOT LIT STREQUAL "")
        run("${RUNTIME}" ${lit_args} "${OUT}/lit_vulkan.ppm" --backend vulkan)
        # Intel Arc's Windows GL and Vulkan drivers light a few pixels on the
        # 32 px tile seams differently (38 of 921600, max diff 13, same state
        # hash; Intel GL against NVIDIA Vulkan: 1; docs/WINDOWS.md). NVIDIA's
        # two match exactly on Linux. A seam-sized allowance, not a tolerance
        # for real lighting differences.
        run("${RHI_PARITY}" compare "${OUT}/lit_gl.ppm" "${OUT}/lit_vulkan.ppm" --max-fraction 0.0001)
    endif()
else()
    message("Vulkan parity skipped: no display (WAYLAND_DISPLAY/DISPLAY unset) or CUERVO_PARITY_VULKAN=0")
endif()
