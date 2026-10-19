#version 450
// RHI parity scenes: straight vertex color (blend state does the rest).

layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 o_color;

void main() {
    o_color = v_color;
}
