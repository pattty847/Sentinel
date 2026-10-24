#pragma once
#include <cmath>
#include <cstdint>

namespace chart {
// The chart's absolute row grid: bin k covers prices [k, k+1) * binPrice (lower
// inclusive, upper exclusive), as the heatmap bins.
struct LadderBins {
    int64_t lo = 0, hi = 0; // inclusive
};
inline int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
// The bins a drawn price window touches.
inline LadderBins ladderWindow(double minPrice, double maxPrice, double binPrice) {
    return {int64_t(std::floor(minPrice / binPrice)), int64_t(std::floor(maxPrice / binPrice))};
}
} // namespace chart
