#pragma once
#include "JournalReader.hpp"
#include "marketdata/model/TradeData.h"
#include "servermodel/BookRecorder.hpp"
#include "TradeRecordWindow.hpp"

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
    // A capture connection change: a journal boundary (new run or connection
    // id, or a gap), TransportUp, TransportDown or CaptureStopped. Not state:
    // never exported, imported or replayed by itself.
    std::function<void()> onConnectionChange;
    void apply(const JournalRecord& input);
    // Boundary-only, validated before installation; never emits an event.
    nlohmann::json exportState() const;
    void importState(const nlohmann::json&);
    // Last 16K trade occurrences, including duplicates; not serialized.
    static constexpr size_t TradeIdWindowCapacity = TradeRecordWindow::Capacity;
    size_t retainedTradeIds() const { return tradeWindow_.uniqueIds(); }
    // Cold boundary snapshot for candle restore; no hot-path set copy.
    std::unordered_set<std::string> tradeIds() const { return tradeWindow_.ids(); }
    struct TradeSeedStats {
        size_t records = 0, ids = 0, segments = 0;
        uint64_t decodedRecords = 0;
        bool degraded = false;
        double ms = 0;
    };
    // Rebuild only dedupe state; preserves feed state and emits no callbacks.
    // inclusive is true for an already-applied anchor cursor, false for a
    // snapshot fallback or a midnight anchor's not-yet-applied resume cursor.
    TradeSeedStats seedTradeIds(const std::vector<JournalFile>& files,
                               const JournalPos& cursor, bool inclusive);
private:
    std::string product_, run_;
    uint64_t connection_ = 0;
    int64_t lastLocal_ = 0;
    std::optional<uint64_t> sequence_;
    bool anchored_ = false;
    // Retained across run/connection boundaries; reconnect snapshots resend trades.
    TradeRecordWindow tradeWindow_;
    struct PendingTrade {
        Trade trade;
        std::optional<uint64_t> numericId;
        size_t order;
        bool fresh = false;
    };
    std::vector<PendingTrade> frameTrades_; // Reuse storage across frames.
    bool rememberTradeId(const std::string& id);
    void collectTrades(const nlohmann::json&, int64_t local);
    void invalid(int64_t local, const std::string& reason);
};
} // namespace sentinel::roller
