#pragma once
#include "JournalReader.hpp"
#include "marketdata/model/TradeData.h"
#include "servermodel/BookRecorder.hpp"

namespace sentinel::roller {
// Native-price durable book, also used by the journal model tap. Boundary
// serialization preserves doubles exactly; it does not synthesize snapshots.
struct JournalBook {
    bool valid = false;
    int64_t envelopeMs = 0;
    std::map<double,double> bids, asks;
    void clear() { valid = false; bids.clear(); asks.clear(); }
    void apply(int64_t envelope, const std::vector<recording::Level>& levels);
    void snapshot(int64_t envelope, const std::vector<recording::Level>& levels) {
        bids.clear(); asks.clear(); apply(envelope, levels); valid = true;
    }
    nlohmann::json exportState() const;
    static JournalBook fromState(const nlohmann::json&);
};
// Single producer. Parsing and callbacks use receive time, never a host clock.
class JournalFeed {
public:
    explicit JournalFeed(std::string product) : product_(std::move(product)) {}
    std::function<void(int64_t, int64_t, std::vector<recording::Level>)> onSnapshot, onUpdates;
    std::function<void(int64_t, const std::string&)> onInvalid;
    // Aggressor side (Coinbase's maker side flipped once, here).
    std::function<void(const Trade&)> onTrade;
    std::function<void(bool)> onConnection;
    std::function<void(int64_t)> onTick;
    void apply(const JournalRecord& input);
    // Boundary-only, validated before installation; never emits an event.
    nlohmann::json exportState() const;
    void importState(const nlohmann::json&);
private:
    std::string product_, run_;
    uint64_t connection_ = 0;
    int64_t lastLocal_ = 0;
    std::optional<uint64_t> sequence_;
    bool anchored_ = false;
    void invalid(int64_t local, const std::string& reason);
};
} // namespace sentinel::roller
