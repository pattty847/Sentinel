#pragma once
#include "JournalReader.hpp"
#include "marketdata/model/TradeData.h"
#include "servermodel/BookRecorder.hpp"
#include <unordered_set>

namespace sentinel::roller {
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
    // 16K IDs cover many 100-trade reconnect snapshots while capping per-product memory.
    static constexpr size_t TradeIdWindowCapacity = 16'384;
    size_t retainedTradeIds() const { return tradeIds_.size(); }
private:
    std::string product_, run_;
    uint64_t connection_ = 0;
    int64_t lastLocal_ = 0;
    std::optional<uint64_t> sequence_;
    bool anchored_ = false;
    // Retained across run/connection boundaries; reconnect snapshots resend trades.
    std::vector<std::string> tradeIdRing_;
    std::unordered_set<std::string> tradeIds_;
    size_t nextTradeId_ = 0;
    struct PendingTrade {
        Trade trade;
        std::optional<uint64_t> numericId;
        size_t order;
    };
    std::vector<PendingTrade> frameTrades_; // Reuse storage across frames.
    bool rememberTradeId(const std::string& id);
    void invalid(int64_t local, const std::string& reason);
};
} // namespace sentinel::roller
