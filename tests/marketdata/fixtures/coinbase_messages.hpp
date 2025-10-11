#pragma once
#include <nlohmann/json.hpp>
#include <string>

/// Golden JSON fixtures for Coinbase Advanced Trade WebSocket API
/// These are real-world message formats used for deterministic testing
namespace fixtures {

/// Trade message with a single trade event
/// MessageDispatcher expects "trades" array at root level (not inside "events")
/// Note: Coinbase uses lowercase "buy"/"sell", but fastSideDetection expects uppercase
inline nlohmann::json coinbaseTrade(
    const std::string& product_id,
    double price,
    double size,
    const std::string& side = "BUY",
    const std::string& trade_id = "12345",
    const std::string& time = "2025-10-09T12:34:56.789123456Z"
) {
    return nlohmann::json{
        {"channel", "market_trades"},
        {"client_id", ""},
        {"timestamp", time},
        {"sequence_num", 0},
        {"trades", nlohmann::json::array({
            {
                {"trade_id", trade_id},
                {"product_id", product_id},
                {"price", std::to_string(price)},
                {"size", std::to_string(size)},
                {"side", side},
                {"time", time}
            }
        })}
    };
}

/// Order book snapshot (initial state) - Matches Coinbase Advanced Trade format
/// Example from TradeData.h lines 110-135
inline nlohmann::json coinbaseL2Snapshot(
    const std::string& product_id,
    const std::vector<std::pair<double, double>>& bids,  // {price, quantity}
    const std::vector<std::pair<double, double>>& asks,
    const std::string& time = "2023-02-09T20:32:50.714964855Z"
) {
    nlohmann::json updates = nlohmann::json::array();

    for (const auto& [price, qty] : bids) {
        updates.push_back({
            {"side", "bid"},
            {"event_time", "1970-01-01T00:00:00Z"},  // Coinbase quirk: event_time is epoch for snapshots
            {"price_level", std::to_string(price)},
            {"new_quantity", std::to_string(qty)}
        });
    }

    for (const auto& [price, qty] : asks) {
        updates.push_back({
            {"side", "offer"},  // Coinbase uses "offer", we normalize to "ask"
            {"event_time", "1970-01-01T00:00:00Z"},
            {"price_level", std::to_string(price)},
            {"new_quantity", std::to_string(qty)}
        });
    }

    return nlohmann::json{
        {"channel", "l2_data"},
        {"client_id", ""},
        {"timestamp", time},
        {"sequence_num", 0},
        {"events", nlohmann::json::array({
            {
                {"type", "snapshot"},
                {"product_id", product_id},
                {"updates", updates}
            }
        })}
    };
}

/// Order book update (incremental changes)
inline nlohmann::json coinbaseL2Update(
    const std::string& product_id,
    const std::vector<std::tuple<std::string, double, double>>& updates,  // {side, price, quantity}
    const std::string& time = "2025-10-09T12:34:57.123Z"
) {
    nlohmann::json update_array = nlohmann::json::array();

    for (const auto& [side, price, qty] : updates) {
        update_array.push_back({
            {"side", side},  // "bid" or "offer"
            {"event_time", time},
            {"price_level", std::to_string(price)},
            {"new_quantity", std::to_string(qty)}
        });
    }

    return nlohmann::json{
        {"channel", "l2_data"},
        {"client_id", ""},
        {"timestamp", time},
        {"sequence_num", 1},
        {"events", nlohmann::json::array({
            {
                {"type", "update"},
                {"product_id", product_id},
                {"updates", update_array}
            }
        })}
    };
}

/// Subscription acknowledgment
/// MessageDispatcher expects product_ids at root level
inline nlohmann::json coinbaseSubscriptionAck(
    const std::vector<std::string>& product_ids,
    const std::string& channel = "subscriptions"
) {
    return nlohmann::json{
        {"channel", channel},
        {"client_id", ""},
        {"timestamp", "2025-10-09T12:34:56.000Z"},
        {"sequence_num", 0},
        {"product_ids", product_ids}
    };
}

/// Provider error message
inline nlohmann::json coinbaseError(
    const std::string& message = "Invalid product_id",
    const std::string& reason = "INVALID_ARGUMENT"
) {
    return nlohmann::json{
        {"type", "error"},
        {"message", message},
        {"reason", reason},
        {"timestamp", "2025-10-09T12:34:56.000Z"}
    };
}

/// Malformed JSON (missing required fields)
inline nlohmann::json malformedMessage() {
    return nlohmann::json{
        {"unknown_field", "unexpected_value"}
    };
}

/// Trade with "offer" side (to test normalization to "ask")
inline nlohmann::json coinbaseTradeWithOffer(
    const std::string& product_id,
    double price,
    double size
) {
    return coinbaseTrade(product_id, price, size, "offer");
}

/// Multi-trade batch (realistic high-volume scenario)
inline nlohmann::json coinbaseMultiTrade(
    const std::string& product_id,
    const std::vector<std::tuple<double, double, std::string>>& trades  // {price, size, side}
) {
    nlohmann::json trade_array = nlohmann::json::array();
    int trade_id = 10000;

    for (const auto& [price, size, side] : trades) {
        trade_array.push_back({
            {"trade_id", std::to_string(trade_id++)},
            {"product_id", product_id},
            {"price", std::to_string(price)},
            {"size", std::to_string(size)},
            {"side", side},
            {"time", "2025-10-09T12:34:56.789Z"}
        });
    }

    return nlohmann::json{
        {"channel", "market_trades"},
        {"client_id", ""},
        {"timestamp", "2025-10-09T12:34:56.789Z"},
        {"sequence_num", 0},
        {"trades", trade_array}
    };
}

} // namespace fixtures
