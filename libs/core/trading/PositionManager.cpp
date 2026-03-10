#include "PositionManager.hpp"

#include <cmath>

namespace trading {

double PositionManager::signForSide(OrderSide side) {
    if (side == OrderSide::Buy) {
        return 1.0;
    }
    if (side == OrderSide::Sell) {
        return -1.0;
    }
    return 0.0;
}

Position PositionManager::applyFill(const std::string& symbol, OrderSide side, double qty, double fillPrice, double markPrice) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& pos = m_positions[symbol];
    pos.symbol = symbol;

    const double signedQty = qty * signForSide(side);
    const double priorQty = pos.netQty;
    const double nextQty = priorQty + signedQty;

    if (std::abs(priorQty) < 1e-12 || (priorQty * signedQty > 0.0)) {
        // Opening or adding to position
        const double priorAbs = std::abs(priorQty);
        const double nextAbs = std::abs(nextQty);
        if (nextAbs > 1e-12) {
            pos.avgPrice = ((pos.avgPrice * priorAbs) + (fillPrice * qty)) / (priorAbs + qty);
        } else {
            pos.avgPrice = 0.0;
        }
    } else if ((priorQty * nextQty) < 0.0) {
        // Flip: realize on the closed portion, open new position at fill price
        const double closedQty = std::abs(priorQty);
        const double closedPnl = (fillPrice - pos.avgPrice) * (priorQty > 0.0 ? closedQty : -closedQty);
        pos.realizedPnl += closedPnl;
        pos.avgPrice = fillPrice;
    } else if (std::abs(nextQty) < 1e-12) {
        // Full close: realize entire position
        const double closedPnl = (fillPrice - pos.avgPrice) * priorQty;
        pos.realizedPnl += closedPnl;
        pos.avgPrice = 0.0;
    } else {
        // Partial close: realize proportional PnL
        const double closedQty = qty;
        const double closedPnl = (fillPrice - pos.avgPrice) * (priorQty > 0.0 ? closedQty : -closedQty);
        pos.realizedPnl += closedPnl;
    }

    pos.netQty = nextQty;
    pos.unrealizedPnl = std::abs(nextQty) > 1e-12
                            ? (markPrice - pos.avgPrice) * pos.netQty
                            : 0.0;
    return pos;
}

Position PositionManager::markToMarket(const std::string& symbol, double markPrice) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_positions.find(symbol);
    if (it == m_positions.end()) {
        return Position{symbol, 0.0, 0.0, 0.0, 0.0};
    }
    Position out = it->second;
    out.unrealizedPnl = std::abs(out.netQty) > 1e-12
                            ? (markPrice - out.avgPrice) * out.netQty
                            : 0.0;
    return out;
}

std::optional<Position> PositionManager::get(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_positions.find(symbol);
    if (it == m_positions.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace trading
