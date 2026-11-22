#pragma once
#include "FlowBar.hpp"
#include <array>
#include <span>

namespace sentinel::flow {
using Bytes = std::vector<uint8_t>;
struct DayFile {
    std::string product;
    int64_t dayStartMs = 0;
    int64_t timeframeMs = 0;
    double nativeTick = 0;
    int64_t priceScale = 0, baseScale = 0, quoteScale = 0;
    std::array<uint8_t, 32> sourceFingerprint{};
    std::vector<FlowBar> bars;
    std::vector<uint32_t> revisions; // FLD1 index metadata, parallel to bars.
    bool operator==(const DayFile&) const = default;
};
Bytes encodeFlowBar(const FlowBar& bar); // FLB1
FlowBar decodeFlowBar(std::span<const uint8_t> bytes);
Bytes encodeFlowDay(const DayFile& day); // FLD1, indexed by bucket start.
// ReEncode also applies the writer's day and ordering checks by re-encoding the
// whole day and comparing bytes. BarsOnly skips that pass (each bar and the index
// are still checked); only measurement uses it today.
enum class DayCheck { ReEncode, BarsOnly };
DayFile decodeFlowDay(std::span<const uint8_t> bytes, DayCheck check = DayCheck::ReEncode);
} // namespace sentinel::flow
