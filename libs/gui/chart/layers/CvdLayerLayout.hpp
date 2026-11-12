#pragma once
#include "chart/core/CvdSeries.hpp"
#include "chart/core/FlowNumberFormat.hpp"

namespace chart {
struct CvdStripStyle {
    double fillAlpha = 1.0;
    double dividerPx = 1.0;
    double linePx = 1.5;
    double labelPx = 13.0;
    double labelRowPx = 17.0;
    double paddingPx = 2.0;
    bool operator==(const CvdStripStyle&) const = default;
};
// Extrema describe samples in the visible window, not the zero-inclusive plot scale.
struct CvdGutterValues {
    double min = 0, max = 0, last = 0;
    bool valid = false;
    static CvdGutterValues visible(const CvdSeries& series, double from, double to, int64_t duration) {
        CvdGutterValues out;
        for (int i = 0; i < series.count; ++i) {
            const auto& point = series.points[size_t(i)];
            if (double(point.timeMs) >= to || double(point.timeMs) + duration <= from || !std::isfinite(point.value)) continue;
            if (!out.valid) out.min = out.max = point.value;
            out.min = std::min(out.min, point.value);
            out.max = std::max(out.max, point.value);
            out.last = point.value;
            out.valid = true;
        }
        return out;
    }
    static void format(double value, char (&out)[24]) {
        flowNumber(value, out, value == 0 ? FlowNumberContext::SignedLabel : FlowNumberContext::Footprint);
    }
};
} // namespace chart
