#pragma once
// Heatmap colour palettes of the GPU path (HeatmapTileNode, heatmap_display.frag).
// They keep the colours of the retired legacy renderer (deleted in S8a), so the
// "legacy" names below are the reference the GPU colours were matched to (S6 plan
// section 4, owner decision 4). The 512-texel palette image is sampled with
// linear filtering: bids in the left half, asks in the right.
#include "heatmap/HeatmapChartSettings.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace heatmap::gpu {
struct PaletteStop {
    float position = 0;
    int r = 0, g = 0, b = 0;
};
struct PaletteGradients {
    std::vector<PaletteStop> bid, ask;
    float gamma = 2.0f; // curve applied to the gradient position
};
// Legacy tone mapping of a code's magnitude (heatmap.gamma/contrast/shader_floor).
struct PaletteTone {
    float gamma = 1.05f, contrast = 1.15f, floor = 0.01f;
    bool operator==(const PaletteTone &) const = default;
};
constexpr int kPaletteWidth = 512;
using PaletteTexels = std::array<uint8_t, kPaletteWidth * 4>; // RGBA8, texel i at 4 * i
struct HeatmapPalette {
    PaletteTexels texels{};
    PaletteTone tone;
};

// The gradients the legacy renderer draws before any preset is chosen.
PaletteGradients legacyDefaultGradients();
// The five legacy presets (Electric, Fire, Ocean, Monochrome, Matrix); nullopt otherwise.
std::optional<PaletteGradients> presetGradients(std::string_view name);
// A chart's palette: its preset, or Custom from its bid/ask gradients (gamma 2,
// as the legacy renderer's default; #RRGGBBAA alpha is ignored). An unknown
// preset or an unparsable custom gradient falls back to Electric.
PaletteGradients gradientsFor(const HeatmapChartSettings &settings);
// The palette image (as the legacy renderer built it).
PaletteTexels paletteTexels(const PaletteGradients &gradients);
std::shared_ptr<const HeatmapPalette> makePalette(const PaletteGradients &gradients, PaletteTone tone);
// Code normalization of the recording palette: [loCode, loCode + range] maps to
// [0, 1] (legacy params3.y/z; the GPU DrawStyle codeFloor/codeRange).
struct CodeWindow {
    float floor = 0, range = 1;
};
CodeWindow codeWindow(double sensitivityMin, double sensitivityMax, double sizeFloor = 1e-6,
                      double codesPerOctave = 819.0);

// CPU reference of heatmap_intensity.frag in recording mode (a nonzero code
// without the unknown marker), sampling the palette with linear filtering:
// premultiplied RGBA in [0, 1]. Tests compare both renderers against it.
std::array<float, 4> legacyRecordingColor(uint16_t cell, CodeWindow window, const HeatmapPalette &palette,
                                          float opacity = 1.0f);
} // namespace heatmap::gpu
