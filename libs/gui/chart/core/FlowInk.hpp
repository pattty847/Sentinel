#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace chart {
// Footprint cell fill and number ink. Qt-free so the rules are unit-tested alone.
inline constexpr double kFlowMinFillAlpha = .18;
struct FlowRgb { uint8_t r, g, b; };

// A side's fill opacity: the weakest visible fill at no volume, opaque at the normaliser.
inline double flowFillAlpha(double volume, double normaliser) {
    const double share = std::isfinite(volume) && normaliser > 0 ? std::clamp(volume / normaliser, 0.0, 1.0) : 0;
    return kFlowMinFillAlpha + (1 - kFlowMinFillAlpha) * share;
}
namespace flow_ink {
inline double linear(uint8_t value) {
    static const auto table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i != 256; ++i) {
            const double c = i / 255.0;
            t[size_t(i)] = c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
        }
        return t;
    }();
    return table[value];
}
inline double luminance(FlowRgb c) { return .2126 * linear(c.r) + .7152 * linear(c.g) + .0722 * linear(c.b); }
inline double contrast(FlowRgb a, FlowRgb b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}
inline uint8_t over(uint8_t top, uint8_t below, double alpha) {
    return uint8_t(std::lround(top * alpha + below * (1 - alpha)));
}
} // namespace flow_ink
// True when the light ink reads better than the dark ink on a fill of this
// colour and opacity drawn over the background. Ties go to the light ink.
inline bool flowInkIsLight(FlowRgb fill, double alpha, FlowRgb background, FlowRgb light, FlowRgb dark) {
    alpha = std::clamp(alpha, 0.0, 1.0);
    const FlowRgb shown{flow_ink::over(fill.r, background.r, alpha), flow_ink::over(fill.g, background.g, alpha),
                        flow_ink::over(fill.b, background.b, alpha)};
    return flow_ink::contrast(light, shown) >= flow_ink::contrast(dark, shown);
}
} // namespace chart
