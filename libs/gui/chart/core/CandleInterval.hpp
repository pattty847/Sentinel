#pragma once
#include <cstdint>

namespace chart {
constexpr bool candleHistoryFromMemory(int64_t tfMs) {
    return tfMs == 100 || tfMs == 1000;
}
constexpr bool drawsTradeFreeBuckets(int64_t tfMs) {
    return tfMs >= 1000;
}
constexpr bool synthesizesQuietBars(int64_t tfMs) {
    return tfMs >= 1000;
}
// A bar is drawn, and counts for the auto price fit, unless it is a trade-free bucket
// at an interval that draws none.
constexpr bool drawsCandle(int64_t tfMs, double volume) {
    return volume > 0 || drawsTradeFreeBuckets(tfMs);
}
} // namespace chart
