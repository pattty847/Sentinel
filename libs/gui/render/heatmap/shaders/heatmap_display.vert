#version 440
layout(location = 0) out vec2 uv;
layout(std140, binding = 1) uniform Draw {
    mat4 mvp;
    vec4 rect;     // item-space x, y, width, height
    vec4 mapping;  // left column offset, columns across rect, top row offset, rows down rect
    uvec4 dims;    // grid columns, rows
    vec4 style;    // code floor, code range, opacity, unused
};
void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    uv = corner;
    gl_Position = mvp * vec4(rect.xy + corner * rect.zw, 0.0, 1.0);
}
