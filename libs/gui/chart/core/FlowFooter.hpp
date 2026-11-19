#pragma once
#include "chart/core/CvdSeries.hpp"
#include "chart/core/ChartNumberFormat.hpp"
#include "chart/core/ChartTextRule.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chart {
struct FlowFooterTotals {
    double delta = 0, volume = 0;
    bool valid = false;
};
// Logical text size and padding also define the shared bottom exclusion area: the chart
// text rule's defaults (a fixed band other layers reserve, so it does not follow the
// live textMinPx/textPadPx settings).
struct FlowFooterLayout {
    static constexpr double textPx = ChartTextRule::kDefaultMinPx, padding = ChartTextRule::kDefaultPadPx;
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
// Bounds every formatChartNumber Full form with a sign: three significant digits with a
// decimal point and a suffix (1.23k), four digits below 1 (0.123), the >999T cap and the
// <0.001 floor. Independent of live data.
template<class Advance>
double footerLabelWidth(Advance advance) {
    double digit = 0;
    for (char c = '0'; c <= '9'; ++c) digit = std::max(digit, advance(c));
    const double suffix = std::max({advance('k'), advance('M'), advance('B'), advance('T')});
    const double magnitude = std::max({4 * digit + advance('.'), 3 * digit + suffix + advance('.'),
                                      advance('>') + 3 * digit + suffix, advance('<') + 4 * digit + advance('.')});
    return magnitude + std::max(advance('+'), advance('-'));
}
// Gutter labels. The unit after DELTA shows at most kFooterUnitGlyphs characters; a
// longer base asset keeps its first six and ends in "..", never silently cut. The
// CUM label names its reset (the CVD title's rule) so a footer without a strip says it.
constexpr int kFooterUnitGlyphs = 8;
inline void footerUnit(const char* unit, char (&out)[kFooterUnitGlyphs + 1]) {
    int n = 0;
    while (unit[n] && n <= kFooterUnitGlyphs) ++n;
    if (n <= kFooterUnitGlyphs) {
        std::copy_n(unit, n, out);
    } else {
        std::copy_n(unit, kFooterUnitGlyphs - 2, out);
        out[kFooterUnitGlyphs - 2] = out[kFooterUnitGlyphs - 1] = '.';
        n = kFooterUnitGlyphs;
    }
    out[n] = '\0';
}
inline const char* footerResetLabel(CvdReset reset) {
    return reset == CvdReset::Visible ? "view" : reset == CvdReset::UtcDay ? "day" : "sess";
}
inline bool footerPill(double delta) { return std::isfinite(delta) && std::abs(delta) >= .01; }
// Footer values in the chart number format; DELTA and CUM carry a '+' on positives.
inline int footerNumber(double value, char (&out)[24], bool signedValue) {
    return formatChartNumber(value, out, {NumberForm::Full, signedValue, false});
}
} // namespace chart
