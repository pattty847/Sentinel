/*
Sentinel — PriceLadder
Role: the one list of "nice" display ticks shared by the server page builder and
the client band policy, so both pick the same tick for the same zoom.
Ladder: {1, 2, 2.5, 5} x 10^k in quote currency, restricted to integer multiples
of the layer's native tick (so $2.5 appears only on grids that can build it).
All arithmetic is in integer price units (price * priceScale) to avoid float drift.
Threading: pure functions, any thread.
*/
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace recording {

// Smallest ladder tick, in price units, that is >= minUnits and a multiple of
// nativeUnits. Returns nativeUnits when minUnits <= nativeUnits; returns 0 on bad
// input or when no ladder tick fits in int64.
inline int64_t ladderTickUnits(double minUnits, int64_t nativeUnits) {
    if (nativeUnits <= 0 || !std::isfinite(minUnits)) {
        return 0;
    }
    if (minUnits <= static_cast<double>(nativeUnits)) {
        return nativeUnits;
    }
    // Mantissas in tenths so 2.5 stays integral: 10, 20, 25, 50 -> 1, 2, 2.5, 5.
    static constexpr int64_t kMantissaTenths[] = {10, 20, 25, 50};
    int64_t decade = 1;  // 10^k in price units
    for (int k = 0; k < 18; ++k) {
        for (const int64_t m : kMantissaTenths) {
            if (decade > std::numeric_limits<int64_t>::max() / m) {
                return 0;
            }
            const int64_t tenths = m * decade;
            if (tenths % 10 != 0) {
                continue;  // 2.5 at the smallest decade is not a whole unit
            }
            const int64_t units = tenths / 10;
            if (units % nativeUnits == 0 && static_cast<double>(units) >= minUnits) {
                return units;
            }
        }
        decade *= 10;
    }
    return 0;
}

// Convenience in quote currency: smallest ladder tick >= minTick on a native grid.
inline double ladderTick(double minTick, double nativeTick, double priceScale) {
    if (!(nativeTick > 0.0) || !(priceScale > 0.0) || !std::isfinite(minTick)) {
        return 0.0;
    }
    const auto native = static_cast<int64_t>(std::llround(nativeTick * priceScale));
    const int64_t units = ladderTickUnits(minTick * priceScale, native);
    return units > 0 ? static_cast<double>(units) / priceScale : 0.0;
}

} // namespace recording
