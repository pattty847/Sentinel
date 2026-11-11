#pragma once

#include "TradingTypes.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace trading {

struct RunningAlgo {
    std::string algoId;
    std::string symbol;
    bool operator==(const RunningAlgo&) const = default;
};

// The server's whole paper-trading state at one instant. A client replaces its
// trading state from it: an order, position, risk leg or algo that is not listed
// does not exist on the server.
struct TradingSnapshot {
    static constexpr int kSchemaVersion = 1;

    // Identifies one LiveTradingSession; a new value means the server restarted and
    // order ids may repeat.
    std::string epoch;
    // "connect" (sent once to a new connection) or "algo_command" (broadcast after a
    // start or stop was processed; commandId echoes the command's command_id).
    std::string reason;
    std::string commandId;
    std::vector<PositionUpdate> positions;   // every symbol the session has traded
    std::vector<OrderUpdate> openOrders;     // New, Open and Partial orders only
    std::vector<RiskOrderUpdate> riskOrders; // attached TP/SL per symbol
    std::vector<RunningAlgo> runningAlgos;

    const PositionUpdate* positionFor(const std::string& symbol) const;
    const RunningAlgo* runningAlgo(const std::string& algoId) const;
};

bool isActiveOrderStatus(OrderStatus status);

nlohmann::json tradingSnapshotJson(const TradingSnapshot& snapshot);
// nullopt when the type, schema_version or a required array is wrong.
std::optional<TradingSnapshot> parseTradingSnapshotJson(const nlohmann::json& msg);

} // namespace trading
