# `rhi_parity`: one capture per RHI feature on each backend, plus an image
# comparer (DESIGN.md 6, Phase 5 "Vulkan feature parity"). Run by the
# `rhi_parity` test (tests/rhi_parity.cmake).
add_executable(rhi_parity rhi_parity.cpp)
target_link_libraries(rhi_parity PRIVATE cuervo)
cuervo_add_shaders(rhi_parity
    shaders/parity_color.vert
    shaders/parity_color.frag
    shaders/parity_instanced.vert
    shaders/parity_textured.vert
    shaders/parity_textured.frag
)
