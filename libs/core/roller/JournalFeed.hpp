#pragma once
#include "JournalReader.hpp"
#include "marketdata/model/TradeData.h"
#include "servermodel/BookRecorder.hpp"

namespace sentinel::roller {
// Single producer. Parsing and callbacks use receive time, never a host clock.
class JournalFeed {
public:
    explicit JournalFeed(std::string product) : product_(std::move(product)) {}
    std::function<void(int64_t, int64_t, std::vector<recording::Level>)> onSnapshot, onUpdates;
    std::function<void(int64_t, const std::string&)> onInvalid;
    std::function<void(const Trade&)> onTrade;
    std::function<void(bool)> onConnection;
    std::function<void(int64_t)> onTick;
    void apply(const JournalRecord& input);
private:
    std::string product_, run_;
    uint64_t connection_ = 0;
    int64_t lastLocal_ = 0;
    std::optional<uint64_t> sequence_;
    bool anchored_ = false;
    void invalid(int64_t local, const std::string& reason);
};
} // namespace sentinel::roller
