#pragma once
#include "levelstore/ExactDecimal.hpp"
#include <memory>
#include <string>
#include <vector>

namespace recording {
struct ExactSourceLevel {
    bool isBid = false;
    std::string priceText, sizeText;
    sentinel::levelstore::DecimalResult price, size;
};
// Owned source text and journal identity travel with the worker message. No
// pointer into the JSON parser or its producer-thread scratch crosses the queue.
struct ExactSource {
    std::string file, run, priceIncrement, sizeIncrement;
    uint64_t block = 0, sequence = 0;
    uint32_t record = 0;
    bool snapshot = false, malformed = false;
    std::vector<ExactSourceLevel> levels;
};
using ExactSourcePtr = std::shared_ptr<const ExactSource>;
} // namespace recording
