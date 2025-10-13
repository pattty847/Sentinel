#pragma once
#include <variant>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "../model/TradeData.h"
#include "Cpp20Utils.hpp" 

struct TradeEvent { Trade trade; };
struct BookSnapshotEvent { std::string productId; };
struct BookUpdateEvent { std::string productId; };
struct SubscriptionAckEvent { std::vector<std::string> productIds; };
struct ProviderErrorEvent { std::string message; };

using Event = std::variant<TradeEvent, BookSnapshotEvent, BookUpdateEvent, SubscriptionAckEvent, ProviderErrorEvent>;

struct DispatchResult { std::vector<Event> events; };

class MessageDispatcher {
public:
    static DispatchResult parse(const nlohmann::json& j) {
        DispatchResult out;
        if (!j.is_object()) return out;

        const std::string channel = j.value("channel", "");
        const std::string type    = j.value("type", "");

        if (channel == "market_trades") {
            if (j.contains("trades") && j["trades"].is_array()) {
                for (const auto& t : j["trades"]) {
                    Trade trade;
                    trade.product_id = t.value("product_id", "");
                    trade.trade_id   = t.value("trade_id", "");
                    trade.price      = Cpp20Utils::fastStringToDouble(t.value("price", "0"));
                    trade.size       = Cpp20Utils::fastStringToDouble(t.value("size", "0"));
                    const std::string side = t.value("side", "");
                    trade.side = Cpp20Utils::fastSideDetection(side);
                    if (t.contains("time")) {
                        trade.timestamp = Cpp20Utils::parseISO8601(t["time"].get<std::string>());
                    } else {
                        trade.timestamp = std::chrono::system_clock::now();
                    }
                    out.events.emplace_back(TradeEvent{std::move(trade)});
                }
            }
        } else if (channel == "l2_data") {
            // Minimal envelope only; MarketDataCore continues detailed handling
            if (j.contains("events") && j["events"].is_array()) {
                for (const auto& ev : j["events"]) {
                    const std::string evType = ev.value("type", "");
                    const std::string product = ev.value("product_id", "");
                    if (evType == "snapshot") out.events.emplace_back(BookSnapshotEvent{product});
                    else if (evType == "update") out.events.emplace_back(BookUpdateEvent{product});
                }
            }
        } else if (channel == "subscriptions") {
            std::vector<std::string> ids;
            if (j.contains("product_ids") && j["product_ids"].is_array()) {
                for (const auto& id : j["product_ids"]) ids.emplace_back(id.get<std::string>());
            }
            out.events.emplace_back(SubscriptionAckEvent{std::move(ids)});
        } else if (type == "error") {
            const std::string msg = j.value("message", "provider error");
            out.events.emplace_back(ProviderErrorEvent{msg});
        }

        return out;
    }
};
