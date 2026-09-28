/*
Sentinel — HeatmapRowGrouping
Role: picks the display tick for the heatmap. The server records rows at a fixed
base tick; the client merges N base rows into one display row so rows keep a
readable pixel height at any zoom. N follows a 1-2-5 sequence, and display rows
sit on an absolute price grid (multiples of N * baseTick) so bands do not shift
when the recorded price band moves.
Threading: pure functions, any thread.
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace heatmap_rows {

// Largest N the shader merges exactly; beyond this it samples with a stride.
constexpr int kMaxExactRows = 64;
constexpr int kMaxRowGroup = 5000;

// Smallest N in 1, 2, 5, 10, 20, 50, ... with N * pxPerBaseRow >= targetPx.
inline int rowsPerDisplayRow(double pxPerBaseRow, double targetPx) {
    if (!(pxPerBaseRow > 0.0) || !std::isfinite(pxPerBaseRow) || !(targetPx > 0.0)) {
        return 1;
    }
    static constexpr int kSteps[] = {1, 2, 5};
    for (int decade = 1; decade <= kMaxRowGroup; decade *= 10) {
        for (int step : kSteps) {
            const int n = step * decade;
            if (n > kMaxRowGroup) {
                return kMaxRowGroup;
            }
            if (static_cast<double>(n) * pxPerBaseRow >= targetPx) {
                return n;
            }
        }
    }
    return kMaxRowGroup;
}

// Cells stay roughly square: the target row height follows the column width
// (times aspect = row height / column width), never below minRowPx, so a wide
// zoom-out reads as fine lines and a close zoom as big cells with room for text.
inline double targetRowPx(double columnPx, double minRowPx, double aspect, double maxRowPx = 64.0) {
    const double lo = (minRowPx > 0.0) ? minRowPx : 1.0;
    if (!(columnPx > 0.0) || !std::isfinite(columnPx) || !(aspect > 0.0)) {
        return lo;
    }
    return std::clamp(columnPx * aspect, lo, std::max(lo, maxRowPx));
}

// Texture row 0 is the top of the band (maxPrice); row r is price maxPrice - r * tick.
// Returns (absolute tick index of row 0) mod n, in [0, n). Display row boundaries
// fall where (phase - r) is a multiple of n.
inline int rowPhase(double maxPrice, double tickSize, int n) {
    if (n <= 1 || !(tickSize > 0.0) || !std::isfinite(maxPrice)) {
        return 0;
    }
    const auto topIndex = static_cast<int64_t>(std::llround(maxPrice / tickSize));
    const int64_t mod = topIndex % n;
    return static_cast<int>(mod < 0 ? mod + n : mod);
}

// First texture row of the display row that contains row r. The group spans
// [first, first + n). Rows outside [0, height) are the caller's to clip.
inline int groupFirstRow(int r, int n, int phase) {
    if (n <= 1) {
        return r;
    }
    const int rel = phase - r;  // relative absolute index, decreasing with r
    const int bucket = (rel >= 0) ? rel / n : -((-rel + n - 1) / n);
    return phase - bucket * n - (n - 1);
}

} // namespace heatmap_rows
