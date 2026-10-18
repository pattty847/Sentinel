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
} // namespace chart
