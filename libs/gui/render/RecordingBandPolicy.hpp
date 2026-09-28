#pragma once

#include "HeatmapRowGrouping.hpp"
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

// Continuous square-cell target, rounded up to a decimal 1-2-5 step for stable
// zoom triggers. This is a request hint, never a claim about the native grid.
inline double stepTick(double tick) {
    if (!(tick > 0) || !std::isfinite(tick)) return 0;
    const double decade = std::pow(10.0, std::floor(std::log10(tick)));
    for (double step : {1.0, 2.0, 5.0, 10.0})
        if (tick <= decade * step * (1.0 + 1e-12)) return decade * step;
    return 0;
}
inline BandRequest requestBand(const View& view, int64_t tf, double minRowPx, double aspect) {
    if (!view.valid() || tf <= 0) return {};
    const double span = view.maxPrice - view.minPrice;
    const double columnPx = view.widthPx * static_cast<double>(tf) /
                            static_cast<double>(view.endMs - view.startMs);
    const double ideal = stepTick(heatmap_rows::squareCellTick(
        span / view.heightPx, columnPx, minRowPx, aspect));
    // S2 exact display_tick requires native ticks the hello does not expose.
    // Automatic selection on an expanded range asks the server to choose its
    // native 1-2-5 tick. Both sides retain at least half a visible span of margin.
    const double half = std::max(span, ideal * kRows * 0.5);
    const double mid = view.minPrice + span * 0.5;
    if (!std::isfinite(half) || !std::isfinite(mid)) return {};
    const double lo = std::max(0.0, mid - half);
    const double hi = std::max(mid + half, lo + 2.0 * half);
    if (!std::isfinite(hi) || hi > 1e12) return {};
    return {lo, hi, ideal};
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
};
} // namespace recording_view
