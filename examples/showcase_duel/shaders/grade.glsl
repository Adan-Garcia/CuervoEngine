// The showcase's own post-process pass (DESIGN.md 4.19, 11.2b), run last over the bloomed frame:
// a radial chromatic fringe that grows toward the screen edge, a cool/warm split grade and faint
// scanlines. The exports show up as fields in the editor and are set in project.toml under
// [render.post.params]. Build: cuervo-tools shader build examples/showcase_duel
@export float fringe = 0.0035;
@export float scan = 0.06;
@export vec3 shadows = vec3(0.90, 0.97, 1.10);
@export vec3 lights = vec3(1.08, 1.0, 0.92);

void main() {
    vec2 c = v_uv - vec2(0.5);
    float r2 = dot(c, c);
    vec2 off = c * (fringe * (0.4 + 4.0 * r2));
    vec3 col = vec3(texture(u_src, v_uv + off).r,
                    texture(u_src, v_uv).g,
                    texture(u_src, v_uv - off).b);
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col *= mix(shadows, lights, clamp(luma * 1.4, 0.0, 1.0));
    col *= 1.0 - scan * (0.5 + 0.5 * sin(v_screen.y * 720.0 * 3.14159265));
    o_color = vec4(col, 1.0);
}
