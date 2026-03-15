#pragma once

#include "BacktestTypes.hpp"
#include "TradingEngine.hpp"

namespace trading {

class IExecutionModel {
public:
    virtual ~IExecutionModel() = default;

    virtual TradingResult onIntent(TradingEngine& engine, const OrderIntent& intent) = 0;
    virtual TradingResult onMarketEvent(TradingEngine& engine, const MarketEvent& event) = 0;
};

} // namespace trading
