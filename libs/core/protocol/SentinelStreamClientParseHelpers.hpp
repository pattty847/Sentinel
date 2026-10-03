#pragma once

#include <vector>
#include <nlohmann/json.hpp>
#include "SentinelStreamClient.hpp"

namespace protocol::clientparse {

struct VolumeProfileSessionBounds {
    int64_t startMs = 0;
    int64_t endMs = 0;
    bool valid() const { return startMs > 0 && endMs > startMs; }
};

Trade parseTrade(const nlohmann::json& msg);
ServerConfig parseServerConfig(const nlohmann::json& msg);
VolumeProfileSessionBounds parseVolumeProfileSessionBounds(const nlohmann::json& msg);
SentinelStreamClient::CandleBar parseCandleBar(const nlohmann::json& item);
std::vector<OrderBookLevel> parseOrderBookLevels(const nlohmann::json& levels);
std::vector<BookLevelUpdate> parseL2Updates(const nlohmann::json& deltas);

} // namespace protocol::clientparse

