#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D glyphTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 color;
    vec4 params;
};

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 msdf = texture(glyphTex, v_texcoord).rgb;
    float sd = median(msdf.r, msdf.g, msdf.b) - 0.5;
    float pxRange = params.x;

    vec2 unitRange = vec2(pxRange) / vec2(textureSize(glyphTex, 0));
    vec2 screenTexSize = vec2(1.0) / fwidth(v_texcoord);
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
    float alpha = clamp(sd * screenPxRange + 0.5, 0.0, 1.0);

    float outA = color.a * alpha;
    fragColor = vec4(color.rgb * outA, outA);
}
