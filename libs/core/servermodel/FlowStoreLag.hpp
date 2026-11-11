#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace sentinel::flow {
// An unbuilt store and an empty live tape have no reference to measure lag from.
inline std::optional<int64_t> storeLagMs(int64_t clock, int64_t sealedThrough,
                                       int64_t firstLiveMs, int64_t rolloverGraceMs) {
    if (sealedThrough <= 0 && firstLiveMs < 0) return std::nullopt;
    constexpr int64_t hour = 3'600'000;
    const auto reference = std::max(sealedThrough,
        firstLiveMs < 0 ? int64_t{0} : firstLiveMs / hour * hour);
    return std::max<int64_t>(0, clock - rolloverGraceMs) / hour * hour - reference;
}
} // namespace sentinel::flow
