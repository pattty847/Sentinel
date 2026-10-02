#pragma once
#include "RawCapture.hpp"
#include <map>

namespace sentinel::capture {

// Decimal strings are converted to integer ticks/atoms without a double at any
// point. Extra trailing zeroes are accepted; off-grid values are errors.
class DecimalGrid {
public:
    explicit DecimalGrid(const std::string& increment);
    uint64_t atoms(const std::string& value) const;
private:
    struct Decimal { uint64_t mantissa; uint32_t places; };
    static Decimal parse(const std::string& value);
    Decimal m_increment;
};

// Fixed numeric window: at most Window/2 + 2 disjoint intervals, independent
// of run duration. Older observations cannot be deduplicated reliably.
class TradeIdWindow {
public:
    static constexpr uint64_t Window = 10000000;
    bool observe(uint64_t value);
    std::optional<uint64_t> high() const { return m_high; }
    size_t retainedIntervals() const { return m_seen.size(); }
    size_t peakIntervals() const { return m_peak; }
    uint64_t evictions() const { return m_evictions; }
    uint64_t outOfWindow() const { return m_outOfWindow; }
private:
    std::map<uint64_t, uint64_t> m_seen;
    std::optional<uint64_t> m_high;
    size_t m_peak = 0;
    uint64_t m_evictions = 0, m_outOfWindow = 0;
};
struct VerificationReport {
    nlohmann::json json;
    bool ok = false;
    int exitCode(bool strictTrades = false) const {
        return !ok || (strictTrades && !json.at("trade_tape_complete").get<bool>()) ? 2 :
            json.at("complete").get<bool>() ? 0 : 3;
    }
};
// Optional synchronization point for deterministic live-rotation tests: the
// selected file list is fixed before this callback, archive inventory follows.
VerificationReport verify(const QString& fileOrDirectory, const std::function<void()>& afterDiscovery = {});

} // namespace sentinel::capture
