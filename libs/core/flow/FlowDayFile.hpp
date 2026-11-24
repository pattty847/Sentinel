#pragma once
// FLC1 day bytes only: no filesystem, store, or serving work. Big trades are absent.
#include "FlowChunkCodec.hpp"
#include <array>
namespace sentinel::flow::chunks {
// Table entries: 0-23 flow 1m hours, 24 flow 1h day chunk, 25 flow 1d piece, 26-49 trades.big hours.
constexpr size_t DaySections = 50;
constexpr size_t TableEntryBytes = 26;
struct DaySection {
    bool sealed = false;
    uint32_t summaryLength = 0;     // flow sections: the summary prefix; 0 for trades.big
    uint64_t sourcePrefix = 0;      // hours: fingerprint of the RAWL2 prefix through the seal point
    std::vector<uint8_t> payload;
    uint8_t stepExponent = 0;       // sealed flow sections: price_step = 2^k
    bool operator==(const DaySection&) const = default;
};
struct DayFile {
    std::string instrument;
    int64_t dayStartMs = 0;
    FlowGrid grid;
    uint8_t ruleVersion = 1;
    uint32_t sealWindowMs = 300'000;
    uint32_t lateTrades = 0, rejectedTrades = 0, regressions = 0;
    uint64_t ruleIdentity = 0;      // section 7 rule identity the sections were built under
    std::array<DaySection, DaySections> sections;
    bool operator==(const DayFile&) const = default;
};

std::vector<uint8_t> encodeDayFile(const DayFile& day);
DayFile parseDayFile(std::span<const uint8_t> bytes);
} // namespace sentinel::flow::chunks
