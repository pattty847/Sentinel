#pragma once
#include "chart/core/FlowNumberFormat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chart {
struct FlowFooterTotals {
    double delta = 0, volume = 0;
    bool valid = false;
};
// Logical text size and padding also define the shared bottom exclusion area.
struct FlowFooterLayout {
    static constexpr double textPx = 13, padding = 2;
    static constexpr double rowHeight = textPx + 2 * padding;
    static constexpr double height = 3 * rowHeight + 2 * padding;
    double top = 0, bottom = 0;
    bool fits = false;
    static FlowFooterLayout place(double surfaceTop, double surfaceBottom, double stripHeight) {
        const double bottom = surfaceBottom - std::max(0.0, stripHeight);
        return {bottom - height, bottom, bottom - height >= surfaceTop};
    }
    double rowTop(int row) const { return top + padding + row * rowHeight; }
    static int64_t stride(double barWidth, double labelWidth) {
        if (!std::isfinite(barWidth) || barWidth <= 0 || !std::isfinite(labelWidth))
            return std::numeric_limits<int64_t>::max();
        const double count = std::ceil((labelWidth + 2 * padding) / barWidth);
        return int64_t(std::clamp(count, 1.0, double(std::numeric_limits<int64_t>::max() / 2)));
    }
    static bool selected(int64_t timeMs, int64_t timeframeMs, int64_t k) {
        if (timeframeMs <= 0 || k <= 0) return false;
        auto index = timeMs / timeframeMs;
        if (timeMs % timeframeMs < 0) --index;
        return index % k == 0;
    }
};
// Bounds all formatter forms: three significant digits, decimal/suffix,
// the >999B cap, and the unsigned <.01 sentinel. Independent of live data.
template<class Advance>
double footerLabelWidth(Advance advance) {
    double digit = 0;
    for (char c = '0'; c <= '9'; ++c) digit = std::max(digit, advance(c));
    const double suffix = std::max({advance('k'), advance('M'), advance('B')});
    const double magnitude = std::max({3 * digit + advance('.'), 3 * digit + suffix + advance('.'),
                                      advance('>') + 3 * digit + advance('B')});
    const double signedWidth = magnitude + std::max(advance('+'), advance('-'));
    return std::max(signedWidth, advance('<') + advance('.') + advance('0') + advance('1'));
}
inline bool footerPill(double delta) { return std::isfinite(delta) && std::abs(delta) >= .01; }
inline int footerNumber(double value, char (&out)[24], bool signedValue) {
    const int size = flowNumber(value, out, signedValue ? FlowNumberContext::SignedLabel : FlowNumberContext::Footprint);
    if (signedValue && value >= .01 && size) {
        std::move_backward(out, out + size + 1, out + size + 2);
        out[0] = '+';
        return size + 1;
    }
    if (!signedValue && value == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    return size;
}
} // namespace chart
