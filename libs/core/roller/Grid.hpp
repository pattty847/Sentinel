#pragma once
#include "servermodel/BookRecorder.hpp"
#include <nlohmann/json.hpp>

namespace sentinel::roller {
struct Grid {
    double priceScale = 0, nearTick = 0, deepTick = 0, sizeFloor = 0;
    recording::RecorderConfig config(const std::filesystem::path& root) const;
};
// Reference = latest snapshot at/before the day's start, otherwise the first
// snapshot in the day. Nearest 1-2-5 step (arithmetic distance; ties upward).
// Near and deep share the derived tick unless overridden; BTC retains $1/$5.
// Shared near-grid policy for recorder and GUI replicas. Unknown quote increment
// is 0; when supplied, the result is a positive multiple of that increment.
double deriveNearTick(double referencePrice, double quoteIncrement = 0);
Grid deriveGrid(const nlohmann::json& metadata, double referencePrice,
                const nlohmann::json& overrides = nlohmann::json::object());
} // namespace sentinel::roller
