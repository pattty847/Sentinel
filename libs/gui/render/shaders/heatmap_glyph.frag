#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D glyphTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 color;
};

void main() {
    float coverage = texture(glyphTex, v_texcoord).r;
    float alpha = color.a * coverage;
    fragColor = vec4(color.rgb * alpha, alpha);
}
