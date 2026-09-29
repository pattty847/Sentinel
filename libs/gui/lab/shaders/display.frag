#version 440
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std430, binding = 0) readonly buffer Output { vec4 cell[]; };
layout(std140, binding = 1) uniform Params {
    uvec4 dims;
    vec4 timeView;
    ivec4 priceView;
};

// Same recording-mode log code normalization and default cyan/orange palette
// as heatmap_intensity.frag and HeatmapOverlayRenderer::ensurePaletteImage.
vec3 palette(float t, bool ask) {
    if (!ask) {
        if (t < 0.35) return mix(vec3(0,20,25), vec3(0,110,130), t / 0.35) / 255.0;
        if (t < 0.70) return mix(vec3(0,110,130), vec3(0,210,220), (t - 0.35) / 0.35) / 255.0;
        return mix(vec3(0,210,220), vec3(160,255,248), (t - 0.70) / 0.30) / 255.0;
    }
    if (t < 0.30) return mix(vec3(35,5,0), vec3(160,30,10), t / 0.30) / 255.0;
    if (t < 0.60) return mix(vec3(160,30,10), vec3(230,80,0), (t - 0.30) / 0.30) / 255.0;
    if (t < 0.85) return mix(vec3(230,80,0), vec3(255,160,30), (t - 0.60) / 0.25) / 255.0;
    return mix(vec3(255,160,30), vec3(255,230,80), (t - 0.85) / 0.15) / 255.0;
}
void main() {
    uint x = min(uint(clamp(uv.x, 0.0, 0.999999) * float(dims.x)), dims.x - 1u);
    uint y = min(uint(clamp(1.0 - uv.y, 0.0, 0.999999) * float(dims.y)), dims.y - 1u);
    vec4 value = cell[y * dims.x + x];
    if (value.z < 0.5) { fragColor = vec4(0.16, 0.18, 0.22, 0.30); return; }
    bool ask = value.y > value.x;
    float quantity = ask ? value.y : value.x;
    if (quantity <= 0.0) { fragColor = vec4(0.0); return; }
    float code = 1.0 + log2(max(quantity / timeView.z, 1.0)) * timeView.w;
    float magnitude = clamp((code - 6000.0) / 24000.0, 0.0, 1.0);
    if (magnitude <= 0.0) { fragColor = vec4(0.0); return; }
    float adjusted = clamp((max(magnitude, 0.08) - 0.5) * 1.25 + 0.5, 0.0, 1.0);
    fragColor = vec4(palette(adjusted, ask), 1.0);
}
