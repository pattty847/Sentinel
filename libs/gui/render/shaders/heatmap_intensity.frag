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
    float encoded = texture(intensityTex, uv).r;
    if (encoded <= 0.0001) {
        fragColor = vec4(0.0, 0.0, 0.0, params.x);
        return;
    }
    float gamma = params.y;

    // Detect bid vs ask: bytes 0-127 = bid, 128-255 = ask
    float isAsk = step(0.5, encoded);
    
    // Extract magnitude within each half (0.0 to 1.0)
    // Bids: 1-127 → 0.004-0.498 → magnitude 0.008-0.996
    // Asks: 128-255 → 0.502-1.0 → magnitude 0.004-1.0
    float magnitude = mix(encoded * 2.0, (encoded - 0.5) * 2.0, isAsk);
    
    // Apply gamma for brightness control, ensure minimum visibility
    float adjusted = pow(max(magnitude, 0.1), gamma);
    
    // Map to palette: bids use 0.0-0.5, asks use 0.5-1.0
    float u = mix(adjusted * 0.49, 0.51 + adjusted * 0.49, isAsk);

    vec4 color = texture(paletteTex, vec2(u, 0.5));
    fragColor = vec4(color.rgb, color.a * params.x);
}
