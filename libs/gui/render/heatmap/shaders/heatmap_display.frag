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
    vec4 tone;
};
// The chart's palette (HeatmapPalette.hpp): the legacy 512-texel image, bids in
// the left half, asks in the right, sampled with linear filtering.
layout(binding = 2) uniform sampler2D paletteTex;
vec4 shade() {
    // Bins are anchored to absolute time/price; panning inside the grid only
    // changes this mapping, so the picture translates by sub-bin amounts.
    float fx = floor(mapping.x + uv.x * mapping.y);
    float fy = floor(mapping.z + uv.y * mapping.w);
    if ((dims.z & 1u) != 0u) {
        // Whole-chunk tiles (B1): the quad covers exactly the tile's columns, so a
        // pixel centre on its edge (uv a hair outside [0, 1]) still belongs to it.
        // Rows beyond the grid repeat its top and bottom sentinel rows, which
        // hold each column's outside-the-book state.
        fx = clamp(fx, 0.0, float(dims.x) - 1.0);
        fy = clamp(fy, 0.0, float(dims.y) - 1.0);
    } else if (fx < 0.0 || fy < 0.0 || fx >= float(dims.x) || fy >= float(dims.y)) {
        return vec4(0.0);
    }
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
    // Recording-mode tone mapping exactly as heatmap_intensity.frag (A/B parity):
    // [codeFloor, codeFloor + codeRange] -> [0, 1], then gamma, floor, contrast.
    float magnitude = clamp((code - style.x) / max(style.y, 1.0), 0.0, 1.0);
    if (magnitude <= 0.0) return vec4(0.0);
    float adjusted = pow(max(magnitude, tone.z), tone.x);
    adjusted = clamp((adjusted - 0.5) * tone.y + 0.5, 0.0, 1.0);
    bool ask = (cell & 0x8000u) != 0u;
    float u = ask ? (256.5 + adjusted * 255.0) / 512.0 : (0.5 + adjusted * 255.0) / 512.0;
    vec4 color = textureLod(paletteTex, vec2(u, 0.5), 0.0); // no mips; divergent flow
    return vec4(color.rgb * color.a, color.a);
}
void main() {
    // Premultiplied output; style.z is the layer opacity (crossfade).
    fragColor = shade() * style.z;
}
