#pragma once
// trade_rejected, schema 1: a refusal without an order lifecycle transition.
// Tick-triggered risk exits have an empty command_id. A rejection carries its
// reason separately from order state and must never create an active order.
#include "trading/TradingTypes.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace protocol {
inline nlohmann::json tradeRejectionJson(const trading::TradeRejection& rejection) {
    return {{"type", "trade_rejected"}, {"schema_version", 1},
            {"symbol", rejection.symbol}, {"command_id", rejection.commandId},
            {"reason", rejection.reason}, {"algo_id", rejection.algoId},
            {"timestamp_ms", rejection.timestampMs}};
}
inline std::optional<trading::TradeRejection> parseTradeRejection(const nlohmann::json& json) {
    try {
        if (json.at("type") != "trade_rejected" || json.at("schema_version") != 1 ||
            !json.at("timestamp_ms").is_number_integer() ||
            (json.at("timestamp_ms").is_number_unsigned() && json.at("timestamp_ms").get<uint64_t>() > INT64_MAX))
            return std::nullopt;
        trading::TradeRejection result{json.at("symbol").get<std::string>(),
            json.at("command_id").get<std::string>(), json.at("reason").get<std::string>(),
            json.at("algo_id").get<std::string>(), json.at("timestamp_ms").get<int64_t>()};
        if (result.reason.empty()) return std::nullopt;
        return result;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}
} // namespace protocol
