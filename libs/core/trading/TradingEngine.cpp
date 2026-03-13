#include "TradingEngine.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <optional>

namespace trading {

TradingEngine::TradingEngine(PriceResolver resolver, double slippageBps)
    : m_priceResolver(std::move(resolver))
    , m_execution(slippageBps) {}


std::optional<Order> TradingEngine::findOrder(const std::string& orderId) const {
    return m_orders.get(orderId);
}

std::vector<Order> TradingEngine::getOpenOrders(const std::string& symbol, const std::string& algoId) const {
    auto orders = m_orders.getAllActiveForSymbol(symbol);
    if (algoId.empty()) {
        return orders;
    }
    std::vector<Order> filtered;
    filtered.reserve(orders.size());
    for (const auto& o : orders) {
        if (o.algoId == algoId) {
            filtered.push_back(o);
        }
    }
    return filtered;
}

std::optional<Position> TradingEngine::getPosition(const std::string& symbol) const {
    return m_positions.get(symbol);
}

TradingResult TradingEngine::onExternalFill(const std::string& orderId,
                                            double cumulativeFilledQty,
                                            double fillPrice) {
    TradingResult out;
    auto existing = m_orders.get(orderId);
    if (!existing.has_value()) {
        return out;
    }
    if (cumulativeFilledQty < 0.0 || fillPrice <= 0.0) {
        return out;
    }

    Order order = *existing;
    if (order.status == OrderStatus::Canceled || order.status == OrderStatus::Filled) {
        return out;
    }
    if (cumulativeFilledQty < order.filledQty) {
        return out;
    }

    const double cappedFilledQty = std::min(cumulativeFilledQty, order.qty);
    const double deltaFillQty = cappedFilledQty - order.filledQty;
    if (deltaFillQty <= 0.0) {
        return out;
    }

    const double priorNotional = order.avgPrice * order.filledQty;
    const double newNotional = fillPrice * deltaFillQty;
    order.filledQty = cappedFilledQty;
    order.avgPrice = (priorNotional + newNotional) / order.filledQty;
    order.status = (order.filledQty >= order.qty) ? OrderStatus::Filled : OrderStatus::Partial;
    m_orders.upsert(order);

    Position position = m_positions.applyFill(order.symbol, order.side, deltaFillQty, fillPrice, fillPrice);

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty,
                                           order.filledQty, order.qty - order.filledQty, order.avgPrice,
                                           order.limitPrice, order.algoId});
    out.positionUpdates.push_back(PositionUpdate{position.symbol, position.netQty, position.avgPrice,
                                                  position.unrealizedPnl, position.realizedPnl});
    out.pnlSnapshots.push_back(buildSnapshot(order.symbol, fillPrice, now, order.algoId));
    return out;
}

TradingResult TradingEngine::onTick(const std::string& symbol, double lastTradePrice, int64_t timestampMs) {
    TradingResult out;
    if (lastTradePrice <= 0.0) {
        return out;
    }

    // Mark-to-market position update (no fill)
    {
        auto pos = m_positions.markToMarket(symbol, lastTradePrice);
        if (std::abs(pos.netQty) > 1e-12) {
            out.positionUpdates.push_back(PositionUpdate{pos.symbol, pos.netQty, pos.avgPrice,
                                                          pos.unrealizedPnl, pos.realizedPnl});
            out.pnlSnapshots.push_back(buildSnapshot(symbol, lastTradePrice, timestampMs, ""));
        }
    }

    // Check resting limit orders for fills
    auto active = m_orders.getAllActive();
    for (auto& order : active) {
        if (order.symbol != symbol) {
            continue;
        }
        if (order.orderType != OrderType::Limit) {
            continue;
        }
        if (!m_execution.shouldFillLimit(order, lastTradePrice)) {
            continue;
        }
        // Limit order fills at the limit price (no slippage for limits)
        auto fillResult = fillOrder(order, order.limitPrice, lastTradePrice, timestampMs);
        out.orderUpdates.insert(out.orderUpdates.end(),
                                fillResult.orderUpdates.begin(), fillResult.orderUpdates.end());
        out.positionUpdates.insert(out.positionUpdates.end(),
                                   fillResult.positionUpdates.begin(), fillResult.positionUpdates.end());
        out.pnlSnapshots.insert(out.pnlSnapshots.end(),
                                fillResult.pnlSnapshots.begin(), fillResult.pnlSnapshots.end());
    }
    return out;
}

TradingResult TradingEngine::fillOrder(Order& order, double fillPx, double markPrice, int64_t timestampMs) {
    TradingResult out;
    if (order.status == OrderStatus::Canceled || order.status == OrderStatus::Filled) {
        return out;
    }
    const double deltaQty = order.qty - order.filledQty;
    if (deltaQty <= 0.0) {
        return out;
    }

    const double priorNotional = order.avgPrice * order.filledQty;
    const double newNotional = fillPx * deltaQty;
    order.filledQty = order.qty;
    order.avgPrice = (priorNotional + newNotional) / order.filledQty;
    order.status = OrderStatus::Filled;
    m_orders.upsert(order);

    Position position = m_positions.applyFill(order.symbol, order.side, deltaQty, fillPx, markPrice);

    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty,
                                           order.filledQty, 0.0, order.avgPrice, order.limitPrice, order.algoId});
    out.positionUpdates.push_back(PositionUpdate{position.symbol, position.netQty, position.avgPrice,
                                                  position.unrealizedPnl, position.realizedPnl});
    out.pnlSnapshots.push_back(buildSnapshot(order.symbol, markPrice, timestampMs, order.algoId));
    return out;
}

PnlSnapshot TradingEngine::buildSnapshot(const std::string& symbol, double markPrice, int64_t timestampMs, const std::string& algoId) const {
    auto pos = m_positions.markToMarket(symbol, markPrice);
    PnlSnapshot snap;
    snap.symbol = symbol;
    snap.timestampMs = timestampMs;
    snap.unrealizedPnl = pos.unrealizedPnl;
    snap.realizedPnl = pos.realizedPnl;
    snap.totalPnl = pos.realizedPnl + pos.unrealizedPnl;
    snap.algoId = algoId;
    return snap;
}

TradingResult TradingEngine::onCommand(const TradeCommand& command) {
    switch (command.action) {
        case TradeAction::PlaceOrder: return handlePlaceOrder(command);
        case TradeAction::CancelOrder: return handleCancelOrder(command);
        case TradeAction::CancelAll: return handleCancelAll(command);
        case TradeAction::Flatten: return handleFlatten(command);
        default: return {};
    }
}

TradingResult TradingEngine::handlePlaceOrder(const TradeCommand& command) {
    TradingResult out;
    if (command.symbol.empty() || command.qty <= 0.0 || command.side == OrderSide::Unknown) {
        return out;
    }

    Order order;
    order.id = nextOrderId();
    order.symbol = command.symbol;
    order.side = command.side;
    order.orderType = command.orderType;
    order.qty = command.qty;
    order.limitPrice = (command.orderType == OrderType::Limit && command.hasPrice) ? command.price : 0.0;
    order.filledQty = 0.0;
    order.avgPrice = 0.0;
    order.algoId = command.algoId;

    if (command.orderType == OrderType::Limit) {
        if (!command.hasPrice || command.price <= 0.0) {
            return out;
        }
        // Resting limit: acknowledge as Open
        order.status = OrderStatus::Open;
        m_orders.upsert(order);
        out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side,
                                               order.qty, 0.0, order.qty, 0.0, order.limitPrice, order.algoId});
        return out;
    }

    // Market order: fill immediately
    const double lastPrice = m_priceResolver(command.symbol);
    const double fillPrice = m_execution.fillPrice(lastPrice, command.side);
    if (fillPrice <= 0.0) {
        return out;
    }

    order.status = OrderStatus::New;
    m_orders.upsert(order);
    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side,
                                           order.qty, 0.0, order.qty, 0.0, 0.0, order.algoId});

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    auto fillResult = fillOrder(order, fillPrice, fillPrice, now);
    out.orderUpdates.insert(out.orderUpdates.end(), fillResult.orderUpdates.begin(), fillResult.orderUpdates.end());
    out.positionUpdates.insert(out.positionUpdates.end(), fillResult.positionUpdates.begin(), fillResult.positionUpdates.end());
    out.pnlSnapshots.insert(out.pnlSnapshots.end(), fillResult.pnlSnapshots.begin(), fillResult.pnlSnapshots.end());
    return out;
}

TradingResult TradingEngine::handleCancelOrder(const TradeCommand& command) {
    TradingResult out;
    auto existing = m_orders.get(command.targetOrderId);
    if (!existing.has_value()) {
        return out;
    }
    Order order = *existing;
    if (order.status == OrderStatus::Filled || order.status == OrderStatus::Canceled) {
        return out;
    }
    order.status = OrderStatus::Canceled;
    m_orders.upsert(order);
    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty,
                                           order.filledQty, order.qty - order.filledQty, order.avgPrice,
                                           order.limitPrice, order.algoId});
    return out;
}

TradingResult TradingEngine::handleCancelAll(const TradeCommand&) {
    TradingResult out;
    auto active = m_orders.getAllActive();
    for (auto& order : active) {
        order.status = OrderStatus::Canceled;
        m_orders.upsert(order);
        out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty,
                                               order.filledQty, order.qty - order.filledQty, order.avgPrice,
                                               order.limitPrice, order.algoId});
    }
    return out;
}

TradingResult TradingEngine::handleFlatten(const TradeCommand& command) {
    TradingResult out;
    auto pos = m_positions.get(command.symbol);
    if (!pos.has_value() || pos->netQty == 0.0) {
        return out;
    }

    TradeCommand flatten = command;
    flatten.action = TradeAction::PlaceOrder;
    flatten.side = pos->netQty > 0.0 ? OrderSide::Sell : OrderSide::Buy;
    flatten.qty = std::abs(pos->netQty);
    flatten.orderType = OrderType::Market;
    return handlePlaceOrder(flatten);
}

std::string TradingEngine::nextOrderId() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_orderSeq;
    return "ord-" + std::to_string(m_orderSeq);
}

TradeCommand parseTradeCommandJson(const std::string& raw) {
    TradeCommand command;
    auto j = nlohmann::json::parse(raw);
    command.commandId = j.value("command_id", "");
    command.action = tradeActionFromString(j.value("action", ""));
    command.symbol = j.value("symbol", "");
    command.side = orderSideFromString(j.value("side", ""));
    command.orderType = orderTypeFromString(j.value("order_type", "MARKET"));
    command.qty = j.value("qty", 0.0);
    command.timestamp = j.value("timestamp", static_cast<int64_t>(0));
    command.algoId = j.value("algo_id", "");
    if (j.contains("price") && !j["price"].is_null()) {
        command.hasPrice = true;
        command.price = j["price"].get<double>();
    }
    command.targetOrderId = j.value("order_id", "");
    return command;
}

} // namespace trading
