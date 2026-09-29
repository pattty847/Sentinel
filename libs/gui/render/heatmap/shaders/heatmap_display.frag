#version 440
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std430, binding = 0) readonly buffer Output { uint cells[]; };
layout(std140, binding = 1) uniform Draw {
    mat4 mvp;
    vec4 rect;
    vec4 mapping;
    uvec4 dims;
    vec4 style;
};
// Recording-mode log-code normalization and default cyan/orange palette, as in
// heatmap_intensity.frag / HeatmapOverlayRenderer::ensurePaletteImage.
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
vec4 shade() {
    // Bins are anchored to absolute time/price; panning inside the grid only
    // changes this mapping, so the picture translates by sub-bin amounts.
    float fx = floor(mapping.x + uv.x * mapping.y);
    float fy = floor(mapping.z + uv.y * mapping.w);
    if (fx < 0.0 || fy < 0.0 || fx >= float(dims.x) || fy >= float(dims.y)) return vec4(0.0);
    uint cell = cells[uint(fy) * dims.x + uint(fx)];
    uint state = (cell >> 16u) & 3u;
    if (state == 0u) return vec4(0.0); // no data: background shows through
    if (state == 1u) {
        // Loading: static diagonal hatch in screen pixels (never stretches with bins).
        bool stripe = mod(floor((gl_FragCoord.x + gl_FragCoord.y) / 5.0), 2.0) < 1.0;
        vec3 c = stripe ? vec3(0.13, 0.20, 0.30) : vec3(0.07, 0.10, 0.15);
        return vec4(c * 0.8, 0.8);
    }
    if (state == 2u) {
        // Veil (scanned but unproven, or a grid that cannot build this tick):
        // neutral mid-grey with a fine anti-diagonal hatch, distinct from the
        // background, from data, and from the blue loading hatch (other slope).
        bool stripe = mod(floor((gl_FragCoord.x - gl_FragCoord.y) / 3.0), 2.0) < 1.0;
        vec3 c = stripe ? vec3(0.36, 0.36, 0.38) : vec3(0.27, 0.27, 0.29);
        return vec4(c * 0.9, 0.9);
    }
    float code = float(cell & 0x7fffu);
    if (code <= 0.0) return vec4(0.0);
    float magnitude = clamp((code - style.x) / style.y, 0.0, 1.0);
    if (magnitude <= 0.0) return vec4(0.0);
    float adjusted = clamp((max(magnitude, 0.08) - 0.5) * 1.25 + 0.5, 0.0, 1.0);
    return vec4(palette(adjusted, (cell & 0x8000u) != 0u), 1.0);
}
void main() {
    // Premultiplied output; style.z is the layer opacity (crossfade).
    fragColor = shade() * style.z;
}
