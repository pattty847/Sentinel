#include "TradingEngine.hpp"

#include <nlohmann/json.hpp>

#include <cmath>

namespace trading {

TradingEngine::TradingEngine(PriceResolver resolver, double slippageBps)
    : m_priceResolver(std::move(resolver))
    , m_execution(slippageBps) {}

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

    const double lastPrice = m_priceResolver(command.symbol);
    const double fillPrice = m_execution.fillPrice(lastPrice, command.side);
    if (fillPrice <= 0.0) {
        return out;
    }

    Order order;
    order.id = nextOrderId();
    order.symbol = command.symbol;
    order.side = command.side;
    order.orderType = command.orderType;
    order.qty = command.qty;
    order.filledQty = command.qty;
    order.avgPrice = fillPrice;
    order.status = OrderStatus::Filled;
    m_orders.upsert(order);

    Position position = m_positions.applyFill(command.symbol, command.side, command.qty, fillPrice, fillPrice);

    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty, order.filledQty, 0.0, order.avgPrice});
    out.positionUpdates.push_back(PositionUpdate{position.symbol, position.netQty, position.avgPrice, position.unrealizedPnl});
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
    out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty, order.filledQty, order.qty - order.filledQty, order.avgPrice});
    return out;
}

TradingResult TradingEngine::handleCancelAll(const TradeCommand&) {
    TradingResult out;
    auto active = m_orders.getAllActive();
    for (auto& order : active) {
        order.status = OrderStatus::Canceled;
        m_orders.upsert(order);
        out.orderUpdates.push_back(OrderUpdate{order.id, order.symbol, order.status, order.side, order.qty, order.filledQty, order.qty - order.filledQty, order.avgPrice});
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
    if (j.contains("price") && !j["price"].is_null()) {
        command.hasPrice = true;
        command.price = j["price"].get<double>();
    }
    command.targetOrderId = j.value("order_id", "");
    return command;
}

} // namespace trading
