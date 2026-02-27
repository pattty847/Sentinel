#pragma once

#include "TradingTypes.hpp"

namespace trading {

class PaperExecutionAdapter {
public:
    explicit PaperExecutionAdapter(double slippageBps = 0.0)
        : m_slippageBps(slippageBps) {}

    double fillPrice(double lastTradePrice, OrderSide side) const;

private:
    double m_slippageBps = 0.0;
};

} // namespace trading
