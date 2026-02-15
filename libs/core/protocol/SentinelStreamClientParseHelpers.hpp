#pragma once

#include <vector>
#include <nlohmann/json.hpp>
#include "SentinelStreamClient.hpp"

namespace protocol::clientparse {

ServerConfig parseServerConfig(const nlohmann::json& msg);
SentinelStreamClient::CandleBar parseCandleBar(const nlohmann::json& item);
std::vector<OrderBookLevel> parseOrderBookLevels(const nlohmann::json& levels);
std::vector<BookLevelUpdate> parseL2Updates(const nlohmann::json& deltas);

} // namespace protocol::clientparse

