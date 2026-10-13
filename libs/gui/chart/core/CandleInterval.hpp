#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace chart {
// The candle wire carries whole seconds; GUI selections retain milliseconds.
constexpr std::optional<int64_t> candleTimeframeSeconds(int64_t tfMs) {
    if (tfMs <= 0 || tfMs % 1000 != 0) return std::nullopt;
    return tfMs / 1000;
}

constexpr std::optional<int64_t> candleTimeframeMilliseconds(int64_t tfSec) {
    if (tfSec <= 0 || tfSec > std::numeric_limits<int64_t>::max() / 1000) return std::nullopt;
    return tfSec * 1000;
}
} // namespace chart
