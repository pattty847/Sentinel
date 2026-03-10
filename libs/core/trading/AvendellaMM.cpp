#include "AvendellaMM.hpp"

#include <cmath>

namespace trading {

AvendellaMM::AvendellaMM() = default;

void AvendellaMM::start(const std::string& symbol, const AlgoParams& params) {
    m_symbol = symbol;
    m_params = params;
    m_lastMid = 0.0;
    m_bidOrderId.clear();
    m_askOrderId.clear();
    m_bidLive = false;
    m_askLive = false;
    m_running = true;
}

void AvendellaMM::stop() {
    m_running = false;
    m_bidLive = false;
    m_askLive = false;
}

bool AvendellaMM::isStale(double newMid) const {
    if (m_lastMid <= 0.0) {
        return true;
    }
    // Requote if mid moved more than half the spread
    const double threshold = m_lastMid * (m_params.spreadBps / 2.0) / 10000.0;
    return std::abs(newMid - m_lastMid) > threshold;
}

std::vector<TradeCommand> AvendellaMM::onTick(double lastTradePrice, int64_t timestampMs,
                                               const std::vector<Order>& openOrders,
                                               const Position& position) {
    if (!m_running || lastTradePrice <= 0.0) {
        return {};
    }

    std::vector<TradeCommand> cmds;
    const double mid = lastTradePrice;
    const double halfSpread = mid * (m_params.spreadBps / 2.0) / 10000.0;

    // Inventory skew: shift quotes to reduce position risk
    const double skewOffset = position.netQty * m_params.skewBps / 10000.0 * mid;

    const double bidPrice = mid - halfSpread - skewOffset;
    const double askPrice = mid + halfSpread - skewOffset;

    const bool canBid = position.netQty < m_params.maxPositionQty;
    const bool canAsk = position.netQty > -m_params.maxPositionQty;

    // Determine if we need to refresh quotes
    const bool stale = isStale(mid);

    if (stale) {
        // Cancel existing quotes
        if (m_bidLive && !m_bidOrderId.empty()) {
            cmds.push_back(makeCancelOrder(m_bidOrderId, timestampMs));
            m_bidLive = false;
        }
        if (m_askLive && !m_askOrderId.empty()) {
            cmds.push_back(makeCancelOrder(m_askOrderId, timestampMs));
            m_askLive = false;
        }
        m_lastMid = mid;
    }

    // Post new quotes if not live
    if (!m_bidLive && canBid && bidPrice > 0.0) {
        auto cmd = makeLimitOrder(OrderSide::Buy, bidPrice, timestampMs);
        m_bidOrderId = cmd.commandId; // track by commandId until we get the real orderId back
        cmds.push_back(std::move(cmd));
        m_bidLive = true;
    }

    if (!m_askLive && canAsk && askPrice > 0.0) {
        auto cmd = makeLimitOrder(OrderSide::Sell, askPrice, timestampMs);
        m_askOrderId = cmd.commandId;
        cmds.push_back(std::move(cmd));
        m_askLive = true;
    }

    return cmds;
}

void AvendellaMM::onOrderUpdate(const OrderUpdate& update) {
    if (update.status == OrderStatus::Filled || update.status == OrderStatus::Canceled) {
        if (update.orderId == m_bidOrderId) {
            m_bidLive = false;
        }
        if (update.orderId == m_askOrderId) {
            m_askLive = false;
        }
    }
    // Track the real server-assigned orderId after the first open acknowledgement
    if (update.status == OrderStatus::Open) {
        if (update.side == OrderSide::Buy && !m_bidLive) {
            m_bidOrderId = update.orderId;
            m_bidLive = true;
        }
        if (update.side == OrderSide::Sell && !m_askLive) {
            m_askOrderId = update.orderId;
            m_askLive = true;
        }
    }
}

std::string AvendellaMM::nextCmdId() {
    return "avmm-" + std::to_string(++m_cmdSeq);
}

TradeCommand AvendellaMM::makeLimitOrder(OrderSide side, double price, int64_t timestampMs) const {
    TradeCommand cmd;
    cmd.commandId = const_cast<AvendellaMM*>(this)->nextCmdId();
    cmd.action = TradeAction::PlaceOrder;
    cmd.symbol = m_symbol;
    cmd.side = side;
    cmd.orderType = OrderType::Limit;
    cmd.qty = m_params.orderQty;
    cmd.price = price;
    cmd.hasPrice = true;
    cmd.timestamp = timestampMs;
    cmd.algoId = m_id;
    return cmd;
}

TradeCommand AvendellaMM::makeCancelOrder(const std::string& orderId, int64_t timestampMs) const {
    TradeCommand cmd;
    cmd.commandId = const_cast<AvendellaMM*>(this)->nextCmdId();
    cmd.action = TradeAction::CancelOrder;
    cmd.symbol = m_symbol;
    cmd.targetOrderId = orderId;
    cmd.timestamp = timestampMs;
    cmd.algoId = m_id;
    return cmd;
}

} // namespace trading
