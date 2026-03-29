#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D dataTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 neutralColor;
    vec4 bidColor;
    vec4 askColor;
    vec4 tuning;
    vec4 cellDebug;
};

void main() {
    vec2 uv = vec2(fract(v_texcoord.x + tuning.w), v_texcoord.y);
    float encoded = texture(dataTex, uv).r;
    float signedDelta = (encoded - 0.5) * 2.0;
    float magnitudeRaw = abs(signedDelta);
    if (magnitudeRaw <= max(tuning.x, 1.0 / 65535.0)) {
        fragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float magnitude = clamp(magnitudeRaw * max(tuning.y, 0.0), 0.0, 1.0);
    float shaped = pow(magnitude, max(tuning.z, 0.001));
    vec3 polarity = mix(bidColor.rgb, askColor.rgb, step(0.0, signedDelta));
    vec3 mapped = mix(neutralColor.rgb, polarity, shaped);
    float intensity = clamp(shaped, 0.0, 1.0);
    vec4 outColor = vec4(mapped, neutralColor.a * intensity);
    if (cellDebug.x > 0.5) {
        vec2 texSize = vec2(textureSize(dataTex, 0));
        vec2 cellPos = fract(uv * texSize);
        float edgeDist = min(min(cellPos.x, 1.0 - cellPos.x),
                             min(cellPos.y, 1.0 - cellPos.y));
        float borderMask = 1.0 - step(cellDebug.y, edgeDist);
        if (borderMask > 0.0) {
            outColor.rgb = mix(outColor.rgb, vec3(1.0), clamp(cellDebug.z, 0.0, 1.0) * borderMask);
            outColor.a = max(outColor.a, borderMask * clamp(cellDebug.z, 0.0, 1.0));
        }
    }
    fragColor = outColor;
}
