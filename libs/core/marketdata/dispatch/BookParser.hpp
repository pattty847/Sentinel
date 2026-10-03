#pragma once
#include "Cpp20Utils.hpp"
#include "Channels.hpp"
#include "marketdata/model/TradeData.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace sentinel::dispatch {
// One L2 level: side, positive finite price, finite non-negative quantity (0 = remove).
inline bool parseLevel(const nlohmann::json& update, bool& isBid, double& price, double& quantity) {
    if (!update.is_object()) return false;
    const auto side = update.find("side");
    const auto priceIt = update.find("price_level");
    const auto qtyIt = update.find("new_quantity");
    if (side == update.end() || priceIt == update.end() || qtyIt == update.end() ||
        !side->is_string() || !priceIt->is_string() || !qtyIt->is_string()) {
        return false;
    }
    const std::string normalized = side_norm::normalize(side->get<std::string>());
    if (normalized != "bid" && normalized != "ask") return false;
    isBid = (normalized == "bid");
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    price = Cpp20Utils::fastStringToDouble(priceIt->get_ref<const std::string&>(), kNaN);
    quantity = Cpp20Utils::fastStringToDouble(qtyIt->get_ref<const std::string&>(), kNaN);
    return std::isfinite(price) && price > 0.0 && std::isfinite(quantity) && quantity >= 0.0;
}

inline int parseSnapshot(const nlohmann::json& event, std::vector<OrderBookLevel>& bids,
                         std::vector<OrderBookLevel>& asks) {
    if (!event.contains("updates") || !event["updates"].is_array()) throw std::runtime_error("missing L2 updates");
    int malformed = 0;
    for (const auto& update : event["updates"]) {
        bool bid; double price, size;
        if (!parseLevel(update, bid, price, size)) { ++malformed; continue; }
        if (size > 0) (bid ? bids : asks).push_back({price, size});
    }
    return malformed;
}
inline bool parseUpdates(const nlohmann::json& event, std::vector<BookLevelUpdate>& levels) {
    if (!event.contains("updates") || !event["updates"].is_array()) throw std::runtime_error("missing L2 updates");
    levels.clear(); levels.reserve(event["updates"].size());
    for (const auto& update : event["updates"]) {
        bool bid; double price, size;
        if (!parseLevel(update, bid, price, size)) return false;
        levels.push_back({bid, price, size});
    }
    return true;
}
} // namespace sentinel::dispatch
