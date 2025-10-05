#pragma once
#include <variant>
#include <vector>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "marketdata/model/TradeData.h"

struct TradeEvent { Trade trade; };
struct BookSnapshotEvent { std::string productId; };
struct BookUpdateEvent { std::string productId; };
struct SubscriptionAckEvent { std::vector<std::string> productIds; };
struct ProviderErrorEvent { std::string message; };

using Event = std::variant<TradeEvent, BookSnapshotEvent, BookUpdateEvent, SubscriptionAckEvent, ProviderErrorEvent>;

struct DispatchResult { std::vector<Event> events; };

class MessageDispatcher {
public:
    static DispatchResult parse(const nlohmann::json& j);
};


