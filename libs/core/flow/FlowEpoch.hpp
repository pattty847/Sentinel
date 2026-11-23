#pragma once
// Pure per-instrument epoch transitions. The caller persists results atomically.
#include "FlowDayFile.hpp"
#include <optional>
namespace sentinel::flow::chunks {
struct EpochState {
    uint64_t counter = 0, ruleIdentity = 0;
    std::vector<int64_t> pending; // day starts, ascending
    bool operator==(const EpochState&) const = default;
};

struct RuleValues {
    uint8_t builderVersion = 1;
    uint32_t sealWindowMs = 15000;
    uint8_t scaleVersion = 1;
    uint64_t bigFloorQuote = 0;
    uint16_t summaryVersion = 1, rowsVersion = 1, bigVersion = 1;
};
uint64_t ruleIdentity(const RuleValues& rules);
std::string flowDomain(const std::string& instrument);
uint64_t epochOf(const std::string& domain, const EpochState& state);
std::vector<uint8_t> encodeEpochFile(const EpochState& state);
EpochState parseEpochFile(std::span<const uint8_t> bytes);
EpochState startupTransition(EpochState stored, uint64_t currentRule, const std::vector<int64_t>& days);
EpochState startupTransition(std::optional<EpochState> stored, uint64_t currentRule,
                             const std::vector<int64_t>& days, uint64_t nowMs);
bool servable(const EpochState& state, const DayFile& day);
void finishDay(EpochState& state, int64_t dayStartMs);
} // namespace sentinel::flow::chunks
