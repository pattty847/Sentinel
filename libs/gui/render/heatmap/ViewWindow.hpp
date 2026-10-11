#pragma once

namespace heatmap::gpu {

struct ViewWindow {
    double timeLoMs = 0, timeHiMs = 1; // half-open, epoch milliseconds
    double priceLo = 0, priceHi = 1;
    bool operator==(const ViewWindow&) const = default;
};

} // namespace heatmap::gpu
