#include "HeatmapPalette.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <cmath>

namespace heatmap::gpu {
namespace {
struct Rgb {
    int r = 0, g = 0, b = 0;
};
// The legacy ColorGradient::interpolate: QColor channels from a float
// expression, truncated to int.
Rgb interpolate(const std::vector<PaletteStop> &stops, float t) {
    if (stops.empty()) return {};
    if (stops.size() == 1 || t <= stops.front().position) return {stops.front().r, stops.front().g, stops.front().b};
    if (t >= stops.back().position) return {stops.back().r, stops.back().g, stops.back().b};
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const auto &a = stops[i], &b = stops[i + 1];
        if (t >= a.position && t <= b.position) {
            const float localT = (t - a.position) / (b.position - a.position);
            return {int(float(a.r) + float(b.r - a.r) * localT), int(float(a.g) + float(b.g - a.g) * localT),
                    int(float(a.b) + float(b.b - a.b) * localT)};
        }
    }
    return {stops.back().r, stops.back().g, stops.back().b};
}
int hexByte(std::string_view s, size_t at) {
    auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int hi = nibble(s[at]), lo = nibble(s[at + 1]);
    return hi < 0 || lo < 0 ? -1 : hi * 16 + lo;
}
// Settings gradient -> stops; nullopt when a stop is malformed (settings validate
// them, so this only guards hand-edited stores).
std::optional<std::vector<PaletteStop>> stopsFrom(const std::vector<GradientStop> &in) {
    if (in.size() < 2) return std::nullopt;
    std::vector<PaletteStop> out;
    float previous = -1;
    for (const auto &stop : in) {
        const std::string_view c = stop.color;
        if ((c.size() != 7 && c.size() != 9) || c[0] != '#' || !std::isfinite(stop.position) ||
            stop.position < 0 || stop.position > 1 || float(stop.position) <= previous)
            return std::nullopt;
        const int r = hexByte(c, 1), g = hexByte(c, 3), b = hexByte(c, 5);
        if (r < 0 || g < 0 || b < 0 || (c.size() == 9 && hexByte(c, 7) < 0)) return std::nullopt;
        previous = float(stop.position);
        out.push_back({previous, r, g, b});
    }
    return out;
}
} // namespace

PaletteGradients legacyDefaultGradients() {
    // Electric cyan and hot orange (the legacy renderer's built-in stops).
    return {{{0.00f, 0, 20, 25}, {0.35f, 0, 110, 130}, {0.70f, 0, 210, 220}, {1.00f, 160, 255, 248}},
            {{0.00f, 35, 5, 0}, {0.30f, 160, 30, 10}, {0.60f, 230, 80, 0}, {0.85f, 255, 160, 30}, {1.00f, 255, 230, 80}},
            2.0f};
}

std::optional<PaletteGradients> presetGradients(std::string_view name) {
    if (name == "Electric") // cyan bids, orange asks
        return PaletteGradients{{{0.0f, 0, 0, 0}, {0.3f, 0, 30, 60}, {0.7f, 0, 180, 220}, {1.0f, 0, 255, 255}},
                                {{0.0f, 0, 0, 0}, {0.3f, 60, 20, 0}, {0.7f, 220, 120, 0}, {1.0f, 255, 200, 0}},
                                2.0f};
    if (name == "Fire") // dark red -> orange -> yellow asks; subtle blue bids
        return PaletteGradients{{{0.0f, 0, 0, 0}, {0.5f, 20, 40, 80}, {1.0f, 60, 120, 200}},
                                {{0.0f, 0, 0, 0}, {0.3f, 80, 0, 0}, {0.6f, 200, 60, 0}, {0.85f, 255, 140, 0},
                                 {1.0f, 255, 255, 0}},
                                1.8f};
    if (name == "Ocean") // deep blue -> teal bids; green asks
        return PaletteGradients{{{0.0f, 0, 0, 0}, {0.3f, 0, 20, 80}, {0.7f, 0, 100, 180}, {1.0f, 0, 220, 255}},
                                {{0.0f, 0, 0, 0}, {0.4f, 0, 60, 40}, {0.8f, 0, 180, 100}, {1.0f, 0, 255, 160}},
                                2.2f};
    if (name == "Monochrome") // white bids, grey asks
        return PaletteGradients{{{0.0f, 0, 0, 0}, {0.5f, 80, 80, 80}, {1.0f, 220, 220, 220}},
                                {{0.0f, 0, 0, 0}, {0.5f, 50, 50, 50}, {1.0f, 140, 140, 140}},
                                2.0f};
    if (name == "Matrix") // green only
        return PaletteGradients{{{0.0f, 0, 0, 0}, {0.4f, 0, 40, 0}, {1.0f, 0, 255, 0}},
                                {{0.0f, 0, 0, 0}, {0.4f, 0, 20, 0}, {1.0f, 0, 180, 0}},
                                1.6f};
    return std::nullopt;
}

PaletteGradients gradientsFor(const HeatmapChartSettings &settings) {
    if (settings.palettePreset == "Custom") {
        auto bid = stopsFrom(settings.bidGradient), ask = stopsFrom(settings.askGradient);
        if (bid && ask) return {std::move(*bid), std::move(*ask), 2.0f};
    } else if (auto preset = presetGradients(settings.palettePreset)) {
        return std::move(*preset);
    }
    return *presetGradients("Electric");
}

PaletteTexels paletteTexels(const PaletteGradients &gradients) {
    PaletteTexels out{};
    const float gamma = gradients.gamma;
    for (int i = 0; i < kPaletteWidth; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kPaletteWidth - 1);
        const bool isAsk = i >= kPaletteWidth / 2;
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float x = std::clamp(localT, 0.0f, 1.0f);
        const float curve = std::pow(x, gamma);
        const Rgb c = interpolate(isAsk ? gradients.ask : gradients.bid, curve);
        out[size_t(i) * 4 + 0] = uint8_t(std::clamp(c.r, 0, 255));
        out[size_t(i) * 4 + 1] = uint8_t(std::clamp(c.g, 0, 255));
        out[size_t(i) * 4 + 2] = uint8_t(std::clamp(c.b, 0, 255));
        out[size_t(i) * 4 + 3] = 255;
    }
    return out;
}

std::shared_ptr<const HeatmapPalette> makePalette(const PaletteGradients &gradients, PaletteTone tone) {
    auto out = std::make_shared<HeatmapPalette>();
    out->texels = paletteTexels(gradients);
    out->tone = tone;
    return out;
}

CodeWindow codeWindow(double sensitivityMin, double sensitivityMax, double sizeFloor, double codesPerOctave) {
    const recording::SizeScale scale{sizeFloor, codesPerOctave};
    const float lo = float(recording::encodeSize(sensitivityMin, scale));
    const float hi = float(recording::encodeSize(sensitivityMax, scale));
    return {lo, std::max(hi - lo, 1.0f)};
}

std::array<float, 4> legacyRecordingColor(uint16_t cell, CodeWindow window, const HeatmapPalette &palette,
                                          float opacity) {
    const int code = cell & 0x7fff;
    const bool ask = (cell & 0x8000) != 0;
    if (code == 0) return {0, 0, 0, 0};
    const float magnitude = std::clamp((float(code) - window.floor) / std::max(window.range, 1.0f), 0.0f, 1.0f);
    if (magnitude <= 0) return {0, 0, 0, 0};
    const auto &tone = palette.tone;
    float adjusted = std::pow(std::max(magnitude, tone.floor), tone.gamma);
    adjusted = std::clamp((adjusted - 0.5f) * tone.contrast + 0.5f, 0.0f, 1.0f);
    const float u = ask ? 0.51f + adjusted * 0.49f : adjusted * 0.49f;
    // Linear filtering, clamp to edge.
    const float coord = u * float(kPaletteWidth) - 0.5f;
    const int i0 = int(std::floor(coord));
    const float f = coord - float(i0);
    const int a = std::clamp(i0, 0, kPaletteWidth - 1), b = std::clamp(i0 + 1, 0, kPaletteWidth - 1);
    std::array<float, 4> out{};
    for (int c = 0; c < 4; ++c) {
        const float lo = palette.texels[size_t(a) * 4 + c] / 255.0f, hi = palette.texels[size_t(b) * 4 + c] / 255.0f;
        out[c] = lo + (hi - lo) * f;
    }
    const float alpha = out[3] * opacity;
    return {out[0] * alpha, out[1] * alpha, out[2] * alpha, alpha};
}
} // namespace heatmap::gpu
