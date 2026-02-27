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
        const double priorAbs = std::abs(priorQty);
        const double nextAbs = std::abs(nextQty);
        if (nextAbs > 1e-12) {
            pos.avgPrice = ((pos.avgPrice * priorAbs) + (fillPrice * qty)) / (priorAbs + qty);
        } else {
            pos.avgPrice = 0.0;
        }
    } else if ((priorQty * nextQty) < 0.0) {
        pos.avgPrice = fillPrice;
    } else if (std::abs(nextQty) < 1e-12) {
        pos.avgPrice = 0.0;
    }

    pos.netQty = nextQty;
    pos.unrealizedPnl = (markPrice - pos.avgPrice) * pos.netQty;
    return pos;
}

Position PositionManager::markToMarket(const std::string& symbol, double markPrice) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_positions.find(symbol);
    if (it == m_positions.end()) {
        return Position{symbol, 0.0, 0.0, 0.0};
    }
    Position out = it->second;
    out.unrealizedPnl = (markPrice - out.avgPrice) * out.netQty;
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
