#pragma once
#include "SparseColumns.hpp"
#include "../servermodel/PriceLadder.hpp"
#include <optional>
#include <string_view>

namespace heatmap {
// The smallest price-ladder tick that is at least minRowPx tall. Defaults retain
// the BTC-USD near-grid policy; callers with other instruments supply their grid.
inline double idealTick(double minPrice, double maxPrice, double heightPx, double minRowPx,
                        double finestTick = 1.0, double priceScale = 100.0) {
    if (!std::isfinite(minPrice) || !std::isfinite(maxPrice) || maxPrice <= minPrice ||
        !std::isfinite(heightPx) || heightPx <= 0 || !std::isfinite(minRowPx) || minRowPx <= 0 ||
        !std::isfinite(finestTick) || finestTick <= 0 || !std::isfinite(priceScale) || priceScale <= 0 ||
        finestTick * priceScale >= 0x1p63) return 0;
    return recording::ladderTick((maxPrice - minPrice) / heightPx * minRowPx, finestTick, priceScale);
}
inline std::string_view layerFor(double tick, double deepTick, int64_t tfMs) {
    return tfMs >= kHourMs || tick >= deepTick ? "deep" : "near";
}
inline constexpr std::array<int64_t, 6> kAutoTimeframes{
    kMinuteMs, 5 * kMinuteMs, 15 * kMinuteMs, kHourMs, 4 * kHourMs, kDayMs};
// A column must be at least one pixel wide. nullopt means even 1D cannot fit:
// the controller must clamp the time span rather than silently exceed the limit.
inline std::optional<int64_t> autoTimeframe(int64_t startMs, int64_t endMs, double widthPx) {
    if (endMs <= startMs || !std::isfinite(widthPx) || widthPx < 1) return {};
    const long double span = static_cast<long double>(endMs) - startMs;
    for (const auto tf : kAutoTimeframes)
        if (span / tf <= widthPx) return tf;
    return {};
}
} // namespace heatmap
