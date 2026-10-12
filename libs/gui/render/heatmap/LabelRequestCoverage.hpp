#pragma once
#include <array>
#include <cstdint>

namespace heatmap::gpu {
struct LabelPicture {
    int64_t tf, tick;
    double priceScale;
    uint64_t spanVersion, liveVersion;
};
// The posted margin makes pans within the same picture layout-only.
template<class Request>
bool labelRequestCovers(const Request& p, LabelPicture picture, const std::array<double, 4>& window) {
    const auto [c0, c1, r0, r1] = window;
    return p.tfMs == picture.tf && p.tickUnits == picture.tick && p.priceScale == picture.priceScale &&
        p.spanVersion == picture.spanVersion && p.liveVersion == picture.liveVersion &&
        double(p.firstBucket) <= c0 && double(p.firstBucket + p.columns) >= c1 && double(p.firstBin) <= r0 &&
        double(p.firstBin + p.rows) >= r1;
}
} // namespace heatmap::gpu
