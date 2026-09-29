#pragma once
// Screen-sized output grid anchored to ABSOLUTE bins: column k is the UTC epoch
// bucket [k * tf, (k + 1) * tf), row bin b is the price band [b * tick, (b + 1) * tick).
// A view that stays inside the grid's guard margin is drawn by translating the
// fragment mapping only (sub-bin smooth panning); values never re-bin on a pan.
#include "servermodel/RecordingCodec.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace heatmap::gpu {

struct ViewWindow {
    double timeLoMs = 0, timeHiMs = 1; // half-open, epoch milliseconds
    double priceLo = 0, priceHi = 1;
    bool operator==(const ViewWindow &) const = default;
};

struct BinGrid {
    int64_t tfMs = 0;
    int64_t firstBucket = 0; // absolute bucket of column 0
    uint32_t columns = 0;
    double displayTick = 0;
    int64_t firstBin = 0;    // absolute price bin of the BOTTOM row
    uint32_t rows = 0;
    bool operator==(const BinGrid &) const = default;
};

struct DisplayMapping {
    float timeOffset = 0, timeSpan = 1;   // columns from grid left to view left, columns across view
    float priceOffset = 0, priceSpan = 1; // rows from grid top to view top, rows down the view
};

inline constexpr uint32_t kMaxGridColumns = 16384, kMaxGridRows = 16384;

// Covers the view plus `margin` bins on each side. nullopt if the view is
// degenerate or the grid would exceed the output limits.
inline std::optional<BinGrid> planGrid(const ViewWindow &view, int64_t tfMs, double displayTick,
                                       int64_t margin = 2) {
    if (tfMs <= 0 || !(displayTick > 0) || !std::isfinite(displayTick) ||
        !std::isfinite(view.timeLoMs) || !std::isfinite(view.timeHiMs) || !(view.timeHiMs > view.timeLoMs) ||
        !std::isfinite(view.priceLo) || !std::isfinite(view.priceHi) || !(view.priceHi > view.priceLo))
        return std::nullopt;
    const double firstT = std::floor(view.timeLoMs / double(tfMs)) - double(margin);
    const double endT = std::ceil(view.timeHiMs / double(tfMs)) + double(margin);
    const double firstP = std::floor(view.priceLo / displayTick) - double(margin);
    const double endP = std::ceil(view.priceHi / displayTick) + double(margin);
    if (endT - firstT > kMaxGridColumns || endP - firstP > kMaxGridRows ||
        std::abs(firstP) > double(std::numeric_limits<int32_t>::max() / 2) ||
        std::abs(endP) > double(std::numeric_limits<int32_t>::max() / 2))
        return std::nullopt;
    BinGrid grid;
    grid.tfMs = tfMs;
    grid.firstBucket = int64_t(firstT);
    grid.columns = uint32_t(endT - firstT);
    grid.displayTick = displayTick;
    grid.firstBin = int64_t(firstP);
    grid.rows = uint32_t(endP - firstP);
    return grid;
}

// True while the view (plus one guard bin per side) lies inside the grid, so the
// existing binned values can be reused with a new mapping.
inline bool gridCovers(const BinGrid &grid, const ViewWindow &view, int64_t tfMs, double displayTick) {
    if (grid.tfMs != tfMs || grid.displayTick != displayTick || !grid.columns || !grid.rows) return false;
    const double firstT = std::floor(view.timeLoMs / double(tfMs));
    const double endT = std::ceil(view.timeHiMs / double(tfMs));
    const double firstP = std::floor(view.priceLo / displayTick);
    const double endP = std::ceil(view.priceHi / displayTick);
    return firstT >= double(grid.firstBucket + 1) && endT <= double(grid.firstBucket + grid.columns - 1) &&
           firstP >= double(grid.firstBin + 1) && endP <= double(grid.firstBin + grid.rows - 1);
}

inline DisplayMapping mappingFor(const BinGrid &grid, const ViewWindow &view) {
    const double tf = double(grid.tfMs), tick = grid.displayTick;
    DisplayMapping m;
    m.timeOffset = float(view.timeLoMs / tf - double(grid.firstBucket));
    m.timeSpan = float((view.timeHiMs - view.timeLoMs) / tf);
    m.priceOffset = float(double(grid.firstBin + grid.rows) - view.priceHi / tick);
    m.priceSpan = float((view.priceHi - view.priceLo) / tick);
    return m;
}
} // namespace heatmap::gpu
