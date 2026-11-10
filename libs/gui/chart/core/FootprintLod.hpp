#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include "FlowNumberFormat.hpp"

namespace chart {
enum class FootprintStage { Hidden, Cells, Numbers };
inline constexpr double kFootprintTextPx = 13;
// Bundled Roboto Mono advance is 1229/2048 em. Two widest magnitude labels, centre gap,
// outer padding and the left candle pill must all fit before numbers appear.
inline constexpr double kFootprintCandleSpace = 10;
inline constexpr double kFootprintNumbersColumn = 2 * kFlowMagnitudeGlyphs * (1229.0 / 2048) * kFootprintTextPx + 6 + kFootprintCandleSpace;
inline constexpr double kFootprintNumbersRow = kFootprintTextPx + 2;
inline FootprintStage footprintLod(double column, double row, FootprintStage previous = FootprintStage::Hidden) {
    if (!std::isfinite(column) || !std::isfinite(row) || column <= 0 || row <= 0) return FootprintStage::Hidden;
    const double cellsBand = previous == FootprintStage::Hidden ? 0 : 2;
    if (column < 24 - cellsBand || row < 6 - cellsBand) return FootprintStage::Hidden;
    const double numbersBand = previous == FootprintStage::Numbers ? 2 : 0;
    return column >= kFootprintNumbersColumn - numbersBand && row >= kFootprintNumbersRow - numbersBand ? FootprintStage::Numbers : FootprintStage::Cells;
}
// Published alongside the row mailbox on the GUI thread, read during render sync.
// Actual contiguous bar intervals distinguish missing history from an empty price row.
struct FootprintCoverage {
    static constexpr int kMaxRanges = 512;
    struct Range { double from = 0, to = 0; };
    std::array<Range, kMaxRanges> ranges{};
    int count = 0;
    uint64_t revision = 0;
    struct Bar { double from = 0, to = 0, low = 0, high = 0; bool operator==(const Bar&) const = default; };
    std::array<Bar, kMaxRanges> bars{};
    int barCount = 0;
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
    bool enabled = false, numbers = true, spineCandles = true, windowIntensity = false;
    double dustNotional = 2500;
    FootprintStage previous = FootprintStage::Hidden;
    FootprintCoverage coverage;
    template<class Raster> FootprintStage stage(const Raster& raster) {
        previous = raster.valid && raster.dpr > 0
            ? footprintLod(raster.colPxF / raster.dpr, raster.rowPxF / raster.dpr, previous)
            : FootprintStage::Hidden;
        if (!enabled || !cellsSource) return FootprintStage::Hidden;
        return !numbers && previous == FootprintStage::Numbers ? FootprintStage::Cells : previous;
    }
    template<class Raster> bool spine(const Raster& raster) {
        return stage(raster) != FootprintStage::Hidden && spineCandles &&
            raster.tick == coverage.tick && raster.tfMs == coverage.timeframeMs && coverage.barCount > 0;
    }
    template<class Raster> bool spineBar(const Raster& raster, double start) {
        if (!spine(raster)) return false;
        for (int i = 0; i < coverage.barCount; ++i) {
            const auto& bar = coverage.bars[size_t(i)];
            if (bar.from == start && bar.high > raster.drawnMinPrice && bar.low < raster.drawnMaxPrice) return true;
        }
        return false;
    }
};
} // namespace chart
