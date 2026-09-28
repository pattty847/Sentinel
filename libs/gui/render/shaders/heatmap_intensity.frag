#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D intensityTex;
layout(binding = 2) uniform sampler2D paletteTex;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;
    vec4 params2;
    vec4 params3;  // x: value mode (0 legacy normalized, 1 absolute log code), y: lo code, z: hi code
};

// Display tick: params2.y = N base rows per display row, params2.z = row phase
// (absolute tick index of texture row 0, mod N). See HeatmapRowGrouping.hpp.
// A display row shows its strongest base row; the encoded values are
// log-normalized, so they cannot be summed here.
float sampleGroupedRows(vec2 uv) {
    ivec2 size = textureSize(intensityTex, 0);
    int x = clamp(int(floor(uv.x * float(size.x))), 0, size.x - 1);
    int r = clamp(int(floor(uv.y * float(size.y))), 0, size.y - 1);
    int n = int(params2.y + 0.5);
    int phase = int(params2.z + 0.5);
    if (n <= 1) {
        return texelFetch(intensityTex, ivec2(x, r), 0).r;
    }
    int rel = phase - r;
    int bucket = (rel >= 0) ? rel / n : -((-rel + n - 1) / n);
    int first = phase - bucket * n - (n - 1);
    int lo = max(first, 0);
    int hi = min(first + n - 1, size.y - 1);
    int stride = max(1, (hi - lo + 64) / 64);
    float best = 0.0;
    float bestEncoded = 0.0;
    for (int i = 0; i < 64; ++i) {
        int row = lo + i * stride;
        if (row > hi) {
            break;
        }
        float e = texelFetch(intensityTex, ivec2(x, row), 0).r;
        float mag = (e >= 0.5) ? (e - 0.5) : e;
        if (mag > best) {
            best = mag;
            bestEncoded = e;
        }
    }
    return bestEncoded;
}

void main() {
    vec2 uv = vec2(fract(v_texcoord.x + params.w), v_texcoord.y);
    float encoded = sampleGroupedRows(uv);
    float gamma = params.y;
    float contrast = params.z;
    float floorVal = params2.x;
    float isAsk = step(0.5, encoded);
    float magnitude = 0.0;

    if (params3.x > 0.5) {
        // Recording mode: absolute log size code | side bit (RecordingCodec.hpp).
        // 0 = recorded empty, 0x8000 = not recorded (unknown), drawn as a faint veil.
        int raw = int(encoded * 65535.0 + 0.5);
        if (raw == 32768) {
            fragColor = vec4(0.16, 0.18, 0.22, 0.30 * params.x);
            return;
        }
        int code = raw & 32767;
        if (code == 0) {
            fragColor = vec4(0.0, 0.0, 0.0, 0.0);
            return;
        }
        float span = max(params3.z - params3.y, 1.0);
        magnitude = clamp((float(code) - params3.y) / span, 0.0, 1.0);
        if (magnitude <= 0.0) {
            fragColor = vec4(0.0, 0.0, 0.0, 0.0);
            return;
        }
    } else {
        if (encoded <= 0.0001) {
            fragColor = vec4(0.0, 0.0, 0.0, 0.0);
            return;
        }
        // Legacy: per-column log-normalized intensity; bids 0..0.5, asks 0.5..1.
        magnitude = mix(encoded * 2.0, (encoded - 0.5) * 2.0, isAsk);
    }
    
    // Apply gamma for brightness control, ensure minimum visibility
    float adjusted = pow(max(magnitude, floorVal), gamma);
    adjusted = clamp((adjusted - 0.5) * contrast + 0.5, 0.0, 1.0);
    
    // Map to palette: bids use 0.0-0.5, asks use 0.5-1.0
    float u = mix(adjusted * 0.49, 0.51 + adjusted * 0.49, isAsk);

    vec4 color = texture(paletteTex, vec2(u, 0.5));
    fragColor = vec4(color.rgb, color.a * params.x);
}
