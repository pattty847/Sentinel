#pragma once
#include "LevelTimeline.hpp"
namespace sentinel::levelstore {
struct D2Hour {
    BookImage start;
    std::vector<MinuteBlock> blocks;
    bool operator==(const D2Hour &) const = default;
};
// Versioned price-page run store. Recorder metadata preserves message atomicity,
// original scale boundaries, canonical ordering and all source inputs.
std::vector<uint8_t> encodeD2(const D2Hour &);
D2Hour decodeD2(std::span<const uint8_t>);
BookImage endImage(const D2Hour &);
} // namespace sentinel::levelstore
