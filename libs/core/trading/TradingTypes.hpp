#pragma once

#include <cstdint>
#include <string>

namespace trading {

enum class TradeAction {
    PlaceOrder,
    CancelOrder,
    CancelAll,
    Flatten,
    Unknown,
};

enum class OrderSide {
    Buy,
    Sell,
    Unknown,
};

enum class OrderType {
    Market,
    Unknown,
};

enum class OrderStatus {
    New,
    Partial,
    Filled,
    Canceled,
    Rejected,
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
    int64_t timestamp = 0;
    std::string targetOrderId;
};

struct Order {
    std::string id;
    std::string symbol;
    OrderSide side = OrderSide::Unknown;
    OrderType orderType = OrderType::Market;
    double qty = 0.0;
    double filledQty = 0.0;
    double avgPrice = 0.0;
    OrderStatus status = OrderStatus::New;
};

struct Position {
    std::string symbol;
    double netQty = 0.0;
    double avgPrice = 0.0;
    double unrealizedPnl = 0.0;
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
};

struct PositionUpdate {
    std::string symbol;
    double positionQty = 0.0;
    double avgPrice = 0.0;
    double unrealizedPnl = 0.0;
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
