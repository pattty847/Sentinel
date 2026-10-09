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
vec4 hatchColor(uint state) {
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
    return vec4(0.0);
}
vec4 paintMagnitude(float code, bool ask) {
    if (code <= 0.0) return vec4(0.0);
    // Recording-mode tone mapping exactly as heatmap_intensity.frag (A/B parity):
    // [codeFloor, codeFloor + codeRange] -> [0, 1], then gamma, floor, contrast.
    float magnitude = clamp((code - style.x) / max(style.y, 1.0), 0.0, 1.0);
    if (magnitude <= 0.0) return vec4(0.0);
    float adjusted = pow(max(magnitude, tone.z), tone.x);
    adjusted = clamp((adjusted - 0.5) * tone.y + 0.5, 0.0, 1.0);
    float u = ask ? (256.5 + adjusted * 255.0) / 512.0 : (0.5 + adjusted * 255.0) / 512.0;
    vec4 color = textureLod(paletteTex, vec2(u, 0.5), 0.0); // no mips; divergent flow
    return vec4(color.rgb * color.a, color.a);
}
vec4 cellColor(uint cell) {
    uint state = (cell >> 16u) & 3u;
    if (state != 3u) return hatchColor(state);
    return paintMagnitude(float(cell & 0x7fffu), (cell & 0x8000u) != 0u);
}
// Coverage in LINEAR light (whole-pixel smooth zoom, slice A2): the palette RGB and
// the scene-graph target are encoded UNORM, so encoded RGB*f would show a half-covered
// pixel at ~21% of its light. RGB scales by f^(1/tone.w) (tone.w = 2.2), alpha by the
// covered area f.
vec4 cover(vec4 premul, float f) {
    f = clamp(f, 0.0, 1.0);
    if (f <= 0.0) return vec4(0.0);
    if (f >= 1.0) return premul;
    return vec4(premul.rgb * pow(f, 1.0 / tone.w), premul.a * f);
}
// One cell under the pixel centre; whole-chunk tiles clamp to their grid.
vec4 centreCell(float fx, float fy, bool clampCells) {
    if (clampCells) {
        // Whole-chunk tiles (B1): the quad covers exactly the tile's columns, so a
        // pixel centre on its edge (uv a hair outside [0, 1]) still belongs to it.
        // Rows beyond the grid repeat its top and bottom sentinel rows, which
        // hold each column's outside-the-book state.
        fx = clamp(fx, 0.0, float(dims.x) - 1.0);
        fy = clamp(fy, 0.0, float(dims.y) - 1.0);
    } else if (fx < 0.0 || fy < 0.0 || fx >= float(dims.x) || fy >= float(dims.y)) {
        return vec4(0.0);
    }
    return cellColor(cells[uint(fy) * dims.x + uint(fx)]);
}
vec4 shade() {
    // Bins are anchored to absolute time/price; panning inside the grid only
    // changes this mapping, so the picture translates by sub-bin amounts.
    float fc = mapping.x + uv.x * mapping.y;
    float fr = mapping.z + uv.y * mapping.w;
    // Cells per pixel, taken before any divergent branch (derivatives).
    float cpp = max(fwidth(fc), 1e-6);
    float rpp = max(fwidth(fr), 1e-6);
    float fx = floor(fc);
    float fy = floor(fr);
    bool clampCells = (dims.z & 1u) != 0u;
    // At rest (whole device pixels per row and column, edges on pixel edges): the
    // single cell under the pixel centre, the slice A bytes.
    if (tone.w == 0.0) return centreCell(fx, fy, clampCells);
    // A zoom transition (fractional pixels per row and column): every cell under the
    // pixel's footprint contributes its covered area.
    float xlo = fc - 0.5 * cpp, xhi = fc + 0.5 * cpp;
    float ylo = fr - 0.5 * rpp, yhi = fr + 0.5 * rpp;
    if (xlo >= fx && xhi <= fx + 1.0 && ylo >= fy && yhi <= fy + 1.0) return centreCell(fx, fy, clampCells);
    float firstCol = floor(xlo), firstRow = floor(ylo);
    float bidW = 0.0, askW = 0.0, dataW = 0.0, loadingW = 0.0, veilW = 0.0;
    // Transitions keep cells at least about one device pixel (rungs have whole pixels
    // >= 1 and the glide passes between them): at most two cells per axis. Four
    // iterations per axis are a safety cap.
    for (int i = 0; i < 4; ++i) {
        float col = firstCol + float(i);
        if (col >= xhi) break;
        float wx = min(xhi, col + 1.0) - max(xlo, col);
        if (wx <= 0.0) continue;
        if (clampCells) col = clamp(col, 0.0, float(dims.x) - 1.0);
        else if (col < 0.0 || col >= float(dims.x)) continue;
        for (int j = 0; j < 4; ++j) {
            float row = firstRow + float(j);
            if (row >= yhi) break;
            float wy = min(yhi, row + 1.0) - max(ylo, row);
            if (wy <= 0.0) continue;
            if (clampCells) row = clamp(row, 0.0, float(dims.y) - 1.0);
            else if (row < 0.0 || row >= float(dims.y)) continue;
            float w = wx * wy;
            uint cell = cells[uint(row) * dims.x + uint(col)];
            uint state = (cell >> 16u) & 3u;
            float code = float(cell & 0x7fffu);
            if (state == 3u && code > 0.0) {
                if ((cell & 0x8000u) != 0u) askW += w * code;
                else bidW += w * code;
                dataW += w;
            } else if (state == 1u) loadingW += w;
            else if (state == 2u) veilW += w;
        }
    }
    // Blend only non-empty log codes; ties select bids as in heatmap_bin.comp.
    vec4 data = vec4(0.0);
    if (dataW > 0.0) data = paintMagnitude((bidW + askW) / dataW, askW > bidW);
    // Coverage on every state boundary, hatch and empty edges included. Hatches stay
    // in screen coordinates; their pattern is never filtered.
    float area = cpp * rpp;
    return cover(data, dataW / area)
         + cover(hatchColor(1u), loadingW / area)
         + cover(hatchColor(2u), veilW / area);
}
void main() {
    // Premultiplied output; style.z is the layer opacity (crossfade).
    fragColor = shade() * style.z;
}
