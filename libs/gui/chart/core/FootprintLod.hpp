#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>

namespace chart {
enum class FootprintStage { Hidden, Cells, Numbers };
// Entry thresholds are 24/6 and 56/12 logical px. Once entered, retain a stage
// until either dimension falls more than two px below its entry threshold.
inline FootprintStage footprintLod(double column, double row, FootprintStage previous = FootprintStage::Hidden) {
    if (!std::isfinite(column) || !std::isfinite(row) || column <= 0 || row <= 0) return FootprintStage::Hidden;
    const double cellsBand = previous == FootprintStage::Hidden ? 0 : 2;
    if (column < 24 - cellsBand || row < 6 - cellsBand) return FootprintStage::Hidden;
    const double numbersBand = previous == FootprintStage::Numbers ? 2 : 0;
    return column >= 56 - numbersBand && row >= 12 - numbersBand ? FootprintStage::Numbers : FootprintStage::Cells;
}
// Published alongside the row mailbox on the GUI thread, read during render sync.
// Actual contiguous bar intervals distinguish missing history from an empty price row.
struct FootprintCoverage {
    static constexpr int kMaxRanges = 512;
    struct Range { double from = 0, to = 0; };
    std::array<Range, kMaxRanges> ranges{};
    int count = 0;
    double tick = 0, low = 0, high = 0;
    int64_t timeframeMs = 0;
    void add(double from, double to) {
        if (count && from <= ranges[size_t(count - 1)].to)
            ranges[size_t(count - 1)].to = std::max(to, ranges[size_t(count - 1)].to);
        else if (count < kMaxRanges) ranges[size_t(count++)] = {from, to};
    }
    template<class Raster> bool covers(const Raster& r) const {
        if (!r.valid || r.tick != tick || r.tfMs != timeframeMs ||
            r.drawnMinPrice < low || r.drawnMaxPrice > high) return false;
        for (int i = 0; i < count; ++i)
            if (ranges[size_t(i)].from <= r.drawnStartMs && ranges[size_t(i)].to >= r.drawnEndMs) return true;
        return false;
    }
};
// A shared render-owned history makes candle and cell decisions independent of
// item sync order, root recreation, and a temporarily hidden candle item.
struct FootprintPresentation {
    bool cellsSource = false;
    bool enabled = false, numbers = true, hollowCandles = true, windowIntensity = false;
    FootprintStage previous = FootprintStage::Hidden;
    FootprintCoverage coverage;
    template<class Raster> FootprintStage stage(const Raster& raster) {
        previous = raster.valid && raster.dpr > 0
            ? footprintLod(raster.colPxF / raster.dpr, raster.rowPxF / raster.dpr, previous)
            : FootprintStage::Hidden;
        if (!enabled || !cellsSource) return FootprintStage::Hidden;
        return !numbers && previous == FootprintStage::Numbers ? FootprintStage::Cells : previous;
    }
    template<class Raster> bool hollow(const Raster& raster) {
        return stage(raster) != FootprintStage::Hidden && hollowCandles && coverage.covers(raster);
    }
};
// Caller owns the fixed buffer: no QString, locale, or heap work in the label path.
inline int footprintNumber(double value, char (&out)[24]) {
    if (!std::isfinite(value) || value < 0) value = 0;
    if (value >= 999950) return std::snprintf(out, sizeof(out), "%.1fM", value / 1e6);
    if (value >= 999.95) return std::snprintf(out, sizeof(out), "%.1fk", value / 1e3);
    if (value > 0 && value < 1) return std::snprintf(out, sizeof(out), "%.3g", value);
    return std::snprintf(out, sizeof(out), "%.0f", value);
}
} // namespace chart
