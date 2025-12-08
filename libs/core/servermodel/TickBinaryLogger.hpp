#pragma once
#include <string>
#include <vector>
#include "../marketdata/model/TradeData.h"

class TickBinaryLogger {
public:
    void logTrade(const Trade& trade);
    void logBookUpdate(const std::string& symbol, const std::vector<BookDelta>& deltas);
};

