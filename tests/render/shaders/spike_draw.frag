#version 440
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std430, binding = 0) readonly buffer Out { vec4 color[]; };
layout(std140, binding = 1) uniform Draw { mat4 mvp; vec4 rect; uvec4 dims; };
void main() {
    uint x = min(uint(uv.x * float(dims.x)), dims.x - 1u);
    fragColor = color[x];
}
