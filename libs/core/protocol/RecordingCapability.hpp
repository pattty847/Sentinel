#pragma once
#include "../config/ConfigTypes.hpp"
#include <nlohmann/json.hpp>

namespace protocol::recordingwire {
inline nlohmann::json capability(const ServerConfig& cfg, bool available) {
    std::vector<int64_t> timeframes;
    for (const auto tf : cfg.chart.timeframesMs)
        if (tf >= 60'000 && tf % 60'000 == 0 && (tf < 3'600'000 || tf % 3'600'000 == 0))
            timeframes.push_back(tf);
    return {{"available", available}, {"layers", {"near", "deep"}},
            {"timeframes_ms", timeframes}, {"size_floor", cfg.recording.sizeFloor},
            {"codes_per_octave", cfg.recording.codesPerOctave}};
}

} // namespace protocol::recordingwire
