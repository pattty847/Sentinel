#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D intensityTex;
layout(binding = 2) uniform sampler2D paletteTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;
};

void main() {
    vec2 uv = vec2(fract(v_texcoord.x + params.w), v_texcoord.y);
    float intensity = texture(intensityTex, uv).r;
    float contrast = params.z;
    float gamma = params.y;

    float adjusted = clamp((intensity - 0.5) * contrast + 0.5, 0.0, 1.0);
    adjusted = pow(adjusted, gamma);

    float isAsk = step(uv.y, 0.5);
    float u = mix(adjusted * 0.5, 0.5 + adjusted * 0.5, isAsk);

    vec4 color = texture(paletteTex, vec2(u, 0.5));
    fragColor = vec4(color.rgb, color.a * params.x);
}
