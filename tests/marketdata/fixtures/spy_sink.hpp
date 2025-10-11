#pragma once
#include "marketdata/sinks/IMarketDataSink.hpp"
#include "marketdata/model/TradeData.h"
#include <vector>

/// Spy sink that records all calls for verification in tests
class SpySink : public IMarketDataSink {
public:
    void onTrade(const Trade& trade) override {
        trades_.push_back(trade);
    }

    // Accessors for test verification
    const std::vector<Trade>& trades() const { return trades_; }
    size_t tradeCount() const { return trades_.size(); }

    void clear() {
        trades_.clear();
    }

    // Helper: verify last trade properties
    const Trade& lastTrade() const {
        if (trades_.empty()) {
            throw std::runtime_error("SpySink: No trades recorded");
        }
        return trades_.back();
    }

private:
    std::vector<Trade> trades_;
};
