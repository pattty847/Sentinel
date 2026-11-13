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
// Local strip coordinates shared by curve geometry and gutter label placement.
struct CvdStripLayout {
    double height;
    CvdStripStyle style;
    int valueRows() const {
        for (int rows = 3; rows > 0; --rows)
            if (height >= rows * style.labelRowPx + (rows + 1) * style.paddingPx) return rows;
        return 0;
    }
    bool titleFits() const { return height >= 4 * style.labelRowPx + 5 * style.paddingPx; }
    double stroke() const { return std::min(style.linePx, height); }
    double lineY(double fraction) const {
        const double padding = std::min(style.paddingPx, (height - stroke()) * 0.25);
        const double title = titleFits() ? style.labelRowPx + 2 * padding : 0;
        return height - stroke() - padding - std::clamp(fraction, 0.0, 1.0) *
            (height - title - stroke() - 2 * padding);
    }
    double lastTop(double fraction) const {
        if (valueRows() == 0) return 0;
        const double separation = valueRows() == 3 ? style.labelRowPx + style.paddingPx : 0;
        return std::clamp(lineY(fraction) - style.labelRowPx / 2,
                          style.paddingPx + separation,
                          height - style.paddingPx - style.labelRowPx - separation);
    }
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
