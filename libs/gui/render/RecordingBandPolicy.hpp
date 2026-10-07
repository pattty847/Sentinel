#pragma once
// Test-only since S8a: the legacy GUI page path that used this client band policy
// is deleted; RecordingPageTests (ClientBandPreservesIdealTickThroughServerAlignment)
// still checks the server page oracle against it. It goes with the page path in S8b.

#include "../../core/heatmap/HeatmapResolution.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace recording_view {
constexpr int kRows = 2048;
constexpr int kDebounceMs = 150;

struct View {
    int64_t startMs = 0, endMs = 0;
    double minPrice = 0, maxPrice = 0, widthPx = 0, heightPx = 0;
    bool follow = true;
    bool valid() const {
        return endMs > startMs && maxPrice > minPrice && widthPx > 0 && heightPx > 0 &&
            std::isfinite(minPrice) && std::isfinite(maxPrice) &&
            std::isfinite(widthPx) && std::isfinite(heightPx);
    }
};
struct BandRequest {
    double minPrice = 0, maxPrice = 0, idealTick = 0;
    bool valid() const { return maxPrice > minPrice && idealTick > 0; }
};

// Display tick rule (owner decision 2026-09-28): the smallest price-ladder tick
// ({1,2,2.5,5} x 10^k) that is at least minRowPx tall on screen, so a zoomed-out
// heatmap shows long thin order lines and a zoomed-in one reaches the native $1
// grid (where rows grow tall and in-cell text appears). Computed on the finest
// recorded grid; the server maps it onto the chosen layer's grid, and ladder
// values at or above the deep native tick coincide on both grids.
constexpr double kFinestNativeTick = 1.0;  // near layer, quote currency
constexpr double kPriceScale = 100.0;      // price units per 1.0 (BTC-USD cents)
inline double idealTick(const View& view, double minRowPx) {
    if (!view.valid() || !(minRowPx > 0)) return 0;
    return heatmap::idealTick(view.minPrice, view.maxPrice, view.heightPx, minRowPx,
                             kFinestNativeTick, kPriceScale);
}
inline BandRequest requestBand(const View& view, double minRowPx) {
    if (!view.valid()) return {};
    const double span = view.maxPrice - view.minPrice;
    const double ideal = idealTick(view, minRowPx);
    if (!(ideal > 0)) return {};
    // Leave two rows for the server's outward tick-grid alignment. An exact
    // rows*tick span can align to rows+1 cells and force the next ladder tick.
    const double half = std::min(std::max(span, ideal * kRows * 0.5),
                                 ideal * (kRows - 2) * 0.5);
    const double mid = view.minPrice + span * 0.5;
    if (!std::isfinite(half) || !std::isfinite(mid)) return {};
    const double lo = std::max(0.0, mid - half);
    const double hi = lo + 2.0 * half;
    if (!std::isfinite(hi) || hi > 1e12) return {};
    return {lo, hi, ideal};
}
inline int firstPageColumns(const View& view, int64_t tf, int maxColumns) {
    if (!view.valid() || tf <= 0 || maxColumns <= 0) return 0;
    // Include partially visible buckets and a small margin for the follow edge.
    const double visible = std::ceil((static_cast<double>(view.endMs) -
                                      static_cast<double>(view.startMs)) / tf) + 16.0;
    return static_cast<int>(std::clamp(visible, 1.0, static_cast<double>(maxColumns)));
}
inline bool needsReband(const View& view, const BandRequest& active, const BandRequest& wanted) {
    return wanted.valid() && (!active.valid() || view.minPrice < active.minPrice ||
        view.maxPrice > active.maxPrice || wanted.idealTick != active.idealTick);
}
// Pure trailing-edge debounce state. No event-loop or wall-clock dependency.
struct Debounce {
    int64_t changedAtMs = 0;
    bool pending = false;
    void changed(int64_t nowMs) { changedAtMs = nowMs; pending = true; }
    void cancel() { pending = false; }
    bool ready(int64_t nowMs) const { return pending && nowMs - changedAtMs >= kDebounceMs; }
    // Time left until ready(), at least 1 ms: a timer that fires before the clock
    // reaches kDebounceMs must re-arm for this long instead of dropping the change.
    int remainingMs(int64_t nowMs) const {
        const int64_t left = kDebounceMs - (nowMs - changedAtMs);
        return int(left < 1 ? 1 : left > kDebounceMs ? kDebounceMs : left);
    }
};
} // namespace recording_view
