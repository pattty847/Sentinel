#pragma once
#include "D2Codec.hpp"
#include "ExactDecimal.hpp"
#include "MinuteSweep.hpp"
namespace sentinel::levelstore {
struct SummaryLevel {
    int64_t durationMs=0;
    ExactDecimal resolution;
    uint32_t bandBp=0;
    bool twapOnly=false;
    bool operator==(const SummaryLevel&) const = default;
};
struct SummaryNode {
    SummaryLevel level;
    int64_t startMs=0,coveredMs=0;
    uint32_t priceDigits=0,sizeDigits=0;
    int64_t bandLo=0,bandHi=0;
    std::vector<BinRow> rows;
    bool operator==(const SummaryNode&) const = default;
};
std::vector<SummaryLevel> bal1mLevels(const std::string& product);
// Caller supplies complete consecutive hours; every node lies wholly in input.
std::vector<SummaryNode> buildSummary(const std::vector<D2Hour>&,const std::vector<SummaryLevel>&);
std::vector<uint8_t> encodeSummary(const std::vector<SummaryNode>&);
std::vector<SummaryNode> decodeSummary(std::span<const uint8_t>);
}
