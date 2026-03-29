#include "PaperExecutionAdapter.hpp"

namespace trading {

double PaperExecutionAdapter::fillPrice(double lastTradePrice, OrderSide side) const {
    if (lastTradePrice <= 0.0) {
        return 0.0;
    }
    const double slip = m_slippageBps / 10000.0;
    if (side == OrderSide::Buy) {
        return lastTradePrice * (1.0 + slip);
    }
    if (side == OrderSide::Sell) {
        return lastTradePrice * (1.0 - slip);
    }
    return lastTradePrice;
}

bool PaperExecutionAdapter::shouldFillLimit(const Order& order, double lastTradePrice) const {
    if (order.limitPrice <= 0.0 || lastTradePrice <= 0.0) {
        return false;
    }
    if (order.side == OrderSide::Buy) {
        return lastTradePrice <= order.limitPrice;
    }
    if (order.side == OrderSide::Sell) {
        return lastTradePrice >= order.limitPrice;
    }
    return false;
}

} // namespace trading
