#pragma once

#include <cstdint>
#include <string>

namespace trading {

enum class TradeAction {
    PlaceOrder,
    CancelOrder,
    CancelAll,
    Flatten,
    SetAttachedRisk,
    Unknown,
};

enum class OrderSide {
    Buy,
    Sell,
    Unknown,
};

enum class OrderType {
    Market,
    Limit,
    Unknown,
};

enum class OrderStatus {
    New,
    Partial,
    Filled,
    Canceled,
    Rejected,
    Open, // Resting limit order waiting for fill
};

struct TradeCommand {
    std::string commandId;
    TradeAction action = TradeAction::Unknown;
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
    int64_t timestamp = 0;
    std::string targetOrderId;
    // Tag to identify the originating algorithm (empty = manual trade)
    std::string algoId;
};

struct RiskOrderUpdate {
    std::string symbol;
    bool hasTakeProfit = false;
    double takeProfitPrice = 0.0;
    bool hasStopLoss = false;
    double stopLossPrice = 0.0;
};

struct Order {
    std::string id;
    std::string symbol;
    OrderSide side = OrderSide::Unknown;
    OrderType orderType = OrderType::Market;
    double qty = 0.0;
    double limitPrice = 0.0; // valid when orderType == Limit
    double filledQty = 0.0;
    double avgPrice = 0.0;
    OrderStatus status = OrderStatus::New;
    std::string algoId; // empty = manual
};

struct Position {
    std::string symbol;
    double netQty = 0.0;
    double avgPrice = 0.0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0; // cumulative closed PnL
};

struct OrderUpdate {
    std::string orderId;
    std::string symbol;
    OrderStatus status = OrderStatus::New;
    OrderSide side = OrderSide::Unknown;
    double qty = 0.0;
    double filledQty = 0.0;
    double remainingQty = 0.0;
    double avgPrice = 0.0;
    double limitPrice = 0.0;
    std::string algoId;
};

struct PositionUpdate {
    std::string symbol;
    double positionQty = 0.0;
    double avgPrice = 0.0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0;
};

// Snapshot of cumulative PnL at a point in time (for the PnL curve)
struct PnlSnapshot {
    std::string symbol;
    int64_t timestampMs = 0;
    double unrealizedPnl = 0.0;
    double realizedPnl = 0.0;
    double totalPnl = 0.0;
    std::string algoId; // empty = all / manual
};

const char* toString(TradeAction action);
const char* toString(OrderSide side);
const char* toString(OrderType type);
const char* toString(OrderStatus status);

TradeAction tradeActionFromString(const std::string& action);
OrderSide orderSideFromString(const std::string& side);
OrderType orderTypeFromString(const std::string& type);
OrderStatus orderStatusFromString(const std::string& status);

} // namespace trading
