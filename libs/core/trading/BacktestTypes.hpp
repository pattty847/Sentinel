#pragma once

#include "TradingTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading {

enum class MarketEventType {
    Trade,
    Book, // reserved for future order-book replay support
};

struct TradeEvent {
    std::string symbol;
    double price = 0.0;
    double qty = 0.0;
    int64_t timestampMs = 0;
};

struct BookEvent {
    std::string symbol;
    int64_t timestampMs = 0;
};

struct MarketEvent {
    MarketEventType type = MarketEventType::Trade;
    int64_t timestampMs = 0;
    std::optional<TradeEvent> trade;
    std::optional<BookEvent> book;
};

enum class OrderIntentAction {
    PlaceOrder,
    CancelOrder,
    CancelAll,
    Flatten,
    SetAttachedRisk,
};

struct OrderIntent {
    std::string intentId;
    OrderIntentAction action = OrderIntentAction::PlaceOrder;
    std::string symbol;
    OrderSide side = OrderSide::Unknown;
    OrderType orderType = OrderType::Market;
    double qty = 0.0;
    double price = 0.0;
    bool hasPrice = false;
    bool hasTakeProfit = false;
    double takeProfitPrice = 0.0;
    bool hasStopLoss = false;
    double stopLossPrice = 0.0;
    int64_t timestampMs = 0;
    std::string targetOrderId;
    std::string algoId;
};

enum class ExecutionEventType {
    OrderAccepted,
    OrderOpened,
    OrderPartialFill,
    OrderFilled,
    OrderCanceled,
    OrderRejected,
    PositionUpdated,
    PnlSnapshot,
    RiskUpdated,
};

struct ExecutionEvent {
    ExecutionEventType type = ExecutionEventType::OrderAccepted;
    int64_t timestampMs = 0;
    std::string symbol;
    std::string algoId;
    std::optional<OrderUpdate> orderUpdate;
    std::optional<PositionUpdate> positionUpdate;
    std::optional<PnlSnapshot> pnlSnapshot;
    std::optional<RiskOrderUpdate> riskOrderUpdate;
};

struct BrokerSnapshot {
    std::string symbol;
    int64_t timestampMs = 0;
    double lastTradePrice = 0.0;
    std::vector<Order> openOrders;
    std::optional<Position> position;
};

struct BacktestConfig {
    std::string strategyId;
    std::string symbol;
    double initialCash = 0.0;
    double slippageBps = 0.0;
    int64_t startTimestampMs = 0;
    int64_t endTimestampMs = 0;
};

struct BacktestSummary {
    std::string symbol;
    std::string strategyId;
    int64_t startedAtMs = 0;
    int64_t finishedAtMs = 0;
    int64_t eventCount = 0;
    int64_t orderEventCount = 0;
    int64_t fillCount = 0;
    double realizedPnl = 0.0;
    double unrealizedPnl = 0.0;
    double totalPnl = 0.0;
    double maxDrawdown = 0.0;
};

struct BacktestResult {
    BacktestSummary summary;
    std::vector<MarketEvent> marketEvents;
    std::vector<ExecutionEvent> executionEvents;
    std::vector<OrderUpdate> orderLifecycleLog;
    std::vector<OrderUpdate> fillLog;
    std::vector<PositionUpdate> positionTimeline;
    std::vector<PnlSnapshot> pnlCurve;
    std::vector<RiskOrderUpdate> riskOrderLog;
};

inline TradeAction toTradeAction(OrderIntentAction action) {
    switch (action) {
    case OrderIntentAction::PlaceOrder: return TradeAction::PlaceOrder;
    case OrderIntentAction::CancelOrder: return TradeAction::CancelOrder;
    case OrderIntentAction::CancelAll: return TradeAction::CancelAll;
    case OrderIntentAction::Flatten: return TradeAction::Flatten;
    case OrderIntentAction::SetAttachedRisk: return TradeAction::SetAttachedRisk;
    }
    return TradeAction::Unknown;
}

inline TradeCommand toTradeCommand(const OrderIntent& intent) {
    TradeCommand command;
    command.commandId = intent.intentId;
    command.action = toTradeAction(intent.action);
    command.symbol = intent.symbol;
    command.side = intent.side;
    command.orderType = intent.orderType;
    command.qty = intent.qty;
    command.price = intent.price;
    command.hasPrice = intent.hasPrice;
    command.hasTakeProfit = intent.hasTakeProfit;
    command.takeProfitPrice = intent.takeProfitPrice;
    command.hasStopLoss = intent.hasStopLoss;
    command.stopLossPrice = intent.stopLossPrice;
    command.timestamp = intent.timestampMs;
    command.targetOrderId = intent.targetOrderId;
    command.algoId = intent.algoId;
    return command;
}

inline OrderIntent fromTradeCommand(const TradeCommand& command) {
    OrderIntent intent;
    intent.intentId = command.commandId;
    switch (command.action) {
    case TradeAction::PlaceOrder: intent.action = OrderIntentAction::PlaceOrder; break;
    case TradeAction::CancelOrder: intent.action = OrderIntentAction::CancelOrder; break;
    case TradeAction::CancelAll: intent.action = OrderIntentAction::CancelAll; break;
    case TradeAction::Flatten: intent.action = OrderIntentAction::Flatten; break;
    case TradeAction::SetAttachedRisk: intent.action = OrderIntentAction::SetAttachedRisk; break;
    default: intent.action = OrderIntentAction::PlaceOrder; break;
    }
    intent.symbol = command.symbol;
    intent.side = command.side;
    intent.orderType = command.orderType;
    intent.qty = command.qty;
    intent.price = command.price;
    intent.hasPrice = command.hasPrice;
    intent.hasTakeProfit = command.hasTakeProfit;
    intent.takeProfitPrice = command.takeProfitPrice;
    intent.hasStopLoss = command.hasStopLoss;
    intent.stopLossPrice = command.stopLossPrice;
    intent.timestampMs = command.timestamp;
    intent.targetOrderId = command.targetOrderId;
    intent.algoId = command.algoId;
    return intent;
}

} // namespace trading
