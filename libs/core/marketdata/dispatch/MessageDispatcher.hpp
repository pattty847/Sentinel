#pragma once
#include <variant>
#include <optional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "../model/TradeData.h"
#include "Cpp20Utils.hpp" 

struct TradeEvent { Trade trade; };
struct BookSnapshotEvent { std::string productId; };
struct BookUpdateEvent { std::string productId; };
struct SubscriptionAckEvent {
    std::vector<std::string> productIds;
    // Absent means this ack makes no statement about L2 (e.g. trades-only).
    std::optional<std::vector<std::string>> level2ProductIds;
};
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
        } else if (channel == "subscriptions" || type == "subscriptions") {
            SubscriptionAckEvent ack;
            const auto append = [&ack](const nlohmann::json& ids, bool level2) {
                if (!ids.is_array()) return;
                if (level2 && !ack.level2ProductIds) ack.level2ProductIds.emplace();
                for (const auto& id : ids) {
                    if (!id.is_string() || id == "heartbeats") continue;
                    ack.productIds.push_back(id.get<std::string>());
                    if (level2) ack.level2ProductIds->push_back(id.get<std::string>());
                }
            };
            const auto parseChannels = [&append](const nlohmann::json& channels) {
                if (!channels.is_array()) return;
                for (const auto& entry : channels) {
                    if (!entry.is_object() || !entry.contains("product_ids")) continue;
                    const auto name = entry.value("name", entry.value("channel", ""));
                    if (name != "heartbeats") append(entry["product_ids"], name == "level2" || name == "l2_data");
                }
            };
            const auto parseState = [&](const nlohmann::json& state) {
                if (!state.is_object()) return;
                if (state.contains("product_ids")) append(state["product_ids"], false);
                if (state.contains("channels")) parseChannels(state["channels"]);
                if (!state.contains("subscriptions")) return;
                const auto& subs = state["subscriptions"];
                if (subs.is_object()) {
                    for (auto it = subs.begin(); it != subs.end(); ++it) {
                        if (it.key() == "channels") parseChannels(it.value());
                        else if (it.key() != "heartbeats") append(it.value(), it.key() == "level2" || it.key() == "l2_data");
                    }
                } else {
                    parseChannels(subs);
                }
            };
            parseState(j);
            // Advanced Trade acks nest channel -> products under each event.
            if (j.contains("events") && j["events"].is_array())
                for (const auto& event : j["events"]) parseState(event);
            out.events.emplace_back(std::move(ack));
        } else if (type == "error") {
            const std::string msg = j.value("message", "provider error");
            out.events.emplace_back(ProviderErrorEvent{msg});
        }

        return out;
    }
};
