#pragma once
#include "marketdata/model/TradeData.h"

class IMarketDataSink {
public:
    virtual ~IMarketDataSink() = default;
    virtual void onTrade(const Trade& trade) = 0;
};


