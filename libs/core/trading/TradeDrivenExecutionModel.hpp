#pragma once

#include "IExecutionModel.hpp"

namespace trading {

class TradeDrivenExecutionModel final : public IExecutionModel {
public:
    TradingResult onIntent(TradingEngine& engine, const OrderIntent& intent) override;
    TradingResult onMarketEvent(TradingEngine& engine, const MarketEvent& event) override;
};

} // namespace trading
