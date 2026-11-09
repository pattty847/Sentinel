#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace sentinel::roller { struct Grid; }
namespace sentinel::flow {
struct FlowScales {
    double nativeTick = 0;
    int64_t rowTickUnits = 0;
    int64_t priceScale = 0, baseScale = 0, quoteScale = 0;
    bool operator==(const FlowScales&) const = default;
};
// The recorder grid and product metadata are the sole source of live/store scales.
FlowScales flowScales(const roller::Grid& grid, const nlohmann::json& metadata);
} // namespace sentinel::flow
