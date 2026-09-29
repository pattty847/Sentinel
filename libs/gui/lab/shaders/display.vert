#version 440
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
    uv = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
