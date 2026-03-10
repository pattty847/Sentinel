#pragma once

#include "TradingTypes.hpp"

namespace trading {

class PaperExecutionAdapter {
public:
    explicit PaperExecutionAdapter(double slippageBps = 0.0)
        : m_slippageBps(slippageBps) {}

    // Market order fill price: last trade price +/- slippage.
    double fillPrice(double lastTradePrice, OrderSide side) const;

    // Returns true if a resting limit order should fill given the current trade price.
    // Buy limit fills when lastTradePrice <= limitPrice; sell limit fills when >= limitPrice.
    bool shouldFillLimit(const Order& order, double lastTradePrice) const;

private:
    double m_slippageBps = 0.0;
};

} // namespace trading
