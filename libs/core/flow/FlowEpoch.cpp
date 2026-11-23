// Epoch serialization and recovery are independent of disk I/O and clocks.
#include "FlowEpoch.hpp"
#include "FlowChunkPrimitives.hpp"
#include <algorithm>
namespace sentinel::flow::chunks {
namespace {
void validateDays(std::span<const int64_t> days) {
    if (days.size() > UINT32_MAX) throw std::invalid_argument("flow epoch day count");
    for (size_t i = 0; i < days.size(); ++i)
        if (days[i] % 86400000 || (i && days[i - 1] >= days[i]))
            throw std::invalid_argument("flow epoch pending days");
}
}
inline uint64_t fnv1a(std::span<const uint8_t> bytes, uint64_t h = 14695981039346656037ULL) {
    for (auto b : bytes) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
std::string flowDomain(const std::string& instrument) { return "flow:" + instrument; }
uint64_t epochOf(const std::string& domain, const EpochState& e) {
    detail::Out o;
    o.b.assign(domain.begin(), domain.end());
    o.fixed(e.ruleIdentity, 8); o.fixed(e.counter, 8);
    return fnv1a(o.b);
}
// Startup: a changed rule identity, or days left pending by a crash, starts a
// transition. Every day is pending after a rule change; a restart with pending
// days advances the counter again.
EpochState startupTransition(EpochState stored, uint64_t currentRule, const std::vector<int64_t>& days) {
    validateDays(days);
    validateDays(stored.pending);
    if ((stored.ruleIdentity != currentRule || !stored.pending.empty()) && stored.counter == UINT64_MAX)
        throw std::overflow_error("flow epoch counter exhausted");
    if (stored.ruleIdentity != currentRule) {
        stored.ruleIdentity = currentRule;
        stored.pending = days;
        ++stored.counter;
    } else if (!stored.pending.empty()) {
        ++stored.counter;
    }
    return stored;
}
bool servable(const EpochState& e, const DayFile& day) {
    return day.ruleIdentity == e.ruleIdentity &&
           !std::binary_search(e.pending.begin(), e.pending.end(), day.dayStartMs);
}
void finishDay(EpochState& e, int64_t dayStartMs) {
    e.pending.erase(std::remove(e.pending.begin(), e.pending.end(), dayStartMs), e.pending.end());
}

// Canonical rule tuple: u8 builder, u32 W, u8 scale, u64 floor,
// then u16 summary/rows/big payload versions, all little-endian.
uint64_t ruleIdentity(const RuleValues& r) {
    detail::Out out;
    out.fixed(r.builderVersion, 1); out.fixed(r.sealWindowMs, 4); out.fixed(r.scaleVersion, 1);
    out.fixed(r.bigFloorQuote, 8);
    out.fixed(r.summaryVersion, 2); out.fixed(r.rowsVersion, 2); out.fixed(r.bigVersion, 2);
    return fnv1a(out.b);
}
std::vector<uint8_t> encodeEpochFile(const EpochState& state) {
    validateDays(state.pending);
    detail::Out out;
    out.fixed(state.counter, 8); out.fixed(state.ruleIdentity, 8); out.fixed(state.pending.size(), 4);
    for (auto day : state.pending) out.fixed(uint64_t(day), 8);
    out.fixed(crc32(out.b), 4);
    return std::move(out.b);
}
EpochState parseEpochFile(std::span<const uint8_t> bytes) {
    detail::In in{bytes};
    EpochState state;
    state.counter = in.fixed(8); state.ruleIdentity = in.fixed(8);
    const auto n = in.fixed(4);
    if (bytes.size() < 24 || n != (bytes.size() - 24) / 8 || (bytes.size() - 24) % 8)
        detail::In::fail("epoch length");
    detail::In tail{bytes.last(4)};
    if (crc32(bytes.first(bytes.size() - 4)) != tail.fixed(4)) detail::In::fail("epoch crc");
    state.pending.reserve(size_t(n));
    for (size_t i = 0; i < n; ++i) state.pending.push_back(int64_t(in.fixed(8)));
    validateDays(state.pending);
    return state;
}
EpochState startupTransition(std::optional<EpochState> stored, uint64_t currentRule,
                             const std::vector<int64_t>& days, uint64_t nowMs) {
    validateDays(days);
    if (!stored) return {nowMs, currentRule, days};
    validateDays(stored->pending);
    return startupTransition(std::move(*stored), currentRule, days);
}
} // namespace sentinel::flow::chunks
