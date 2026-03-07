#include "TradingTypes.hpp"

namespace trading {

const char* toString(TradeAction action) {
    switch (action) {
        case TradeAction::PlaceOrder: return "PLACE_ORDER";
        case TradeAction::CancelOrder: return "CANCEL_ORDER";
        case TradeAction::CancelAll: return "CANCEL_ALL";
        case TradeAction::Flatten: return "FLATTEN";
        default: return "UNKNOWN";
    }
}

const char* toString(OrderSide side) {
    switch (side) {
        case OrderSide::Buy: return "BUY";
        case OrderSide::Sell: return "SELL";
        default: return "UNKNOWN";
    }
}

const char* toString(OrderType type) {
    switch (type) {
        case OrderType::Market: return "MARKET";
        default: return "UNKNOWN";
    }
}

const char* toString(OrderStatus status) {
    switch (status) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::Partial: return "PARTIAL";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Canceled: return "CANCELED";
        case OrderStatus::Rejected: return "REJECTED";
        default: return "REJECTED";
    }
}

TradeAction tradeActionFromString(const std::string& action) {
    if (action == "PLACE_ORDER") return TradeAction::PlaceOrder;
    if (action == "CANCEL_ORDER") return TradeAction::CancelOrder;
    if (action == "CANCEL_ALL") return TradeAction::CancelAll;
    if (action == "FLATTEN") return TradeAction::Flatten;
    return TradeAction::Unknown;
}

OrderSide orderSideFromString(const std::string& side) {
    if (side == "BUY") return OrderSide::Buy;
    if (side == "SELL") return OrderSide::Sell;
    return OrderSide::Unknown;
}

OrderType orderTypeFromString(const std::string& type) {
    if (type == "MARKET") return OrderType::Market;
    return OrderType::Unknown;
}

OrderStatus orderStatusFromString(const std::string& status) {
    if (status == "NEW") return OrderStatus::New;
    if (status == "PARTIAL") return OrderStatus::Partial;
    if (status == "FILLED") return OrderStatus::Filled;
    if (status == "CANCELED") return OrderStatus::Canceled;
    return OrderStatus::Rejected;
}

} // namespace trading
