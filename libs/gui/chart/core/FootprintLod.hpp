#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include "FlowNumberFormat.hpp"
#include "../../../core/heatmap/ChartSurface.hpp"

namespace chart {
enum class FootprintStage { Hidden, Strip, Cells, Numbers };
inline constexpr double kFootprintTextPx = 13;
// Bundled Roboto Mono advance is 1229/2048 em. Two widest magnitude labels, centre gap,
// outer padding and the left candle pill must all fit before numbers appear.
inline constexpr double kFootprintCandleSpace = 10;
inline constexpr double kFootprintNumbersFitColumn = 2 * kFlowMagnitudeGlyphs * (1229.0 / 2048) * kFootprintTextPx + 6 + kFootprintCandleSpace;
inline constexpr double kFootprintNumbersFitRow = kFootprintTextPx + 2;
inline constexpr double kFootprintNumbersColumn = kFootprintNumbersFitColumn + 2;
inline constexpr double kFootprintNumbersRow = kFootprintNumbersFitRow + 2;
// Logical px per column where Cells starts (bid/ask split and spine). Below it every
// valid positive column draws Strip across exactly its column, never hidden by zoom.
// The narrowest column is the chart's shared zoom-out limit, kMinChartCellLogicalPx
// (heatmap::maxTimeSpanMs), the same column a candle fills; no footprint floor.
inline constexpr double kFootprintCellsColumn = 24;
// A stage is held this far below its entry width.
inline constexpr double kFootprintStageHysteresis = 2;
// Minimum logical height of one drawn band; thinner rows merge into bands.
inline constexpr double kFootprintBandMinRow = 6;
// Every bar of the chart's widest supported view (heatmap::kMaxChartColumns columns,
// partial edges included).
inline constexpr int kFootprintMaxBars = heatmap::kMaxChartSurfaceBars;
// Rows merge into bands, so stages depend on the column width only.
inline FootprintStage footprintLod(double column, double row, FootprintStage previous = FootprintStage::Hidden) {
    if (!std::isfinite(column) || !std::isfinite(row) || column <= 0 || row <= 0) return FootprintStage::Hidden;
    const auto held = [previous](FootprintStage stage, double band) { return previous >= stage ? band : 0.0; };
    if (column < kFootprintCellsColumn - held(FootprintStage::Cells, kFootprintStageHysteresis)) return FootprintStage::Strip;
    return column >= kFootprintNumbersColumn - held(FootprintStage::Numbers, kFootprintStageHysteresis)
        ? FootprintStage::Numbers : FootprintStage::Cells;
}
// A band shrinks back only once the smaller band clears its minimum by this much.
inline constexpr double kFootprintBandHysteresis = 1;
inline int64_t footprintRowsFor(double minimum, double row) {
    return int64_t(std::clamp(std::ceil(minimum / row - 1e-9), 1.0, 1048576.0));
}
// Drawn rows per band: the smallest k with k * row >= the stage's minimum band height,
// holding the previous k across zoom until a smaller k clears the minimum by the margin.
inline int64_t footprintBandRows(double row, FootprintStage stage, int64_t previous = 0) {
    if (stage == FootprintStage::Hidden || !std::isfinite(row) || row <= 0) return 1;
    const double minimum = stage == FootprintStage::Numbers ? kFootprintNumbersRow : kFootprintBandMinRow;
    return std::max(footprintRowsFor(minimum, row),
                    std::min(previous, footprintRowsFor(minimum + kFootprintBandHysteresis, row)));
}
// Bars or rows a view of `cells` columns or rows can touch, partial edges included.
// Callers pass a device-pixel ratio (size / cell px), never a world-bound difference.
inline int64_t footprintSpanCount(double cells) {
    if (!std::isfinite(cells) || cells <= 0) return 1;
    return int64_t(std::min(std::ceil(cells - 1e-9), 1e12)) + 1;
}
// Smallest k whose worst case fits the cell budget: a run of rows spans at most
// ceil(rows / k) + 1 bands in each visible bar.
inline int64_t footprintBudgetBandRows(int64_t visibleBars, int64_t visibleRows, int64_t maxCells) {
    if (visibleBars <= 0 || visibleRows <= 0) return 1;
    const int64_t perBar = std::max<int64_t>(1, maxCells / visibleBars - 1);
    return std::max<int64_t>(1, (visibleRows + perBar - 1) / perBar);
}
// Bands sit on the absolute drawn-row grid, so a pan never re-partitions them.
inline int64_t footprintBandStart(int64_t row, int64_t rows) {
    if (rows <= 1) return row;
    const int64_t band = row / rows - (row % rows < 0 ? 1 : 0);
    return band * rows;
}
// Published alongside the row mailbox on the GUI thread, read during render sync.
// Per-bar price bounds distinguish missing history from an empty price row.
struct FootprintCoverage {
    static constexpr int kMaxBars = kFootprintMaxBars;
    uint64_t revision = 0;
    struct Bar { double from = 0, to = 0, low = 0, high = 0; bool operator==(const Bar&) const = default; };
    std::array<Bar, kMaxBars> bars{};
    int barCount = 0;
    double tick = 0;
    int64_t timeframeMs = 0;
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
        return stage(raster) >= FootprintStage::Cells && spineCandles &&
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
