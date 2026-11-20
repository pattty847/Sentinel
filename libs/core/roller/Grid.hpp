#pragma once
#include "servermodel/BookRecorder.hpp"
#include <nlohmann/json.hpp>

namespace sentinel::roller {
struct Grid {
    double priceScale = 0, nearTick = 0, deepTick = 0, sizeFloor = 0;
    // Null preserves the Coinbase policy. Kalshi retains the exact instrument
    // block for checkpoint identity, including bands with the same finest tick.
    nlohmann::json instrument;
    recording::RecorderConfig config(const std::filesystem::path& root) const;
};
// Reference = latest snapshot at/before the day's start, otherwise the first
// snapshot in the day. Nearest 1-2-5 step (arithmetic distance; ties upward).
// Near and deep share the derived tick unless overridden; BTC retains $1/$5.
Grid deriveGrid(const nlohmann::json& metadata, double referencePrice,
                const nlohmann::json& overrides = nlohmann::json::object());
// Whole 0..1 grid, independent of the book. Exact decimal divisibility only;
// no rounding, relative windows, reference mid or Coinbase overrides.
Grid deriveKalshiGrid(const nlohmann::json& instrument);
} // namespace sentinel::roller
