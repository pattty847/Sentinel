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

inline bool validFlowTimeframe(int64_t tf) {
    return tf == 1000 || (tf >= 60'000 && tf <= 86'400'000 && tf % 60'000 == 0 && 86'400'000 % tf == 0);
}
SentinelStreamClient::FlowBatch parseFlowLive(const nlohmann::json& msg);
SentinelStreamClient::FlowBatch parseFlowHistory(std::span<const uint8_t> frame);

Trade parseTrade(const nlohmann::json& msg);
ServerConfig parseServerConfig(const nlohmann::json& msg);
VolumeProfileSessionBounds parseVolumeProfileSessionBounds(const nlohmann::json& msg);
SentinelStreamClient::CandleBar parseCandleBar(const nlohmann::json& item);
std::vector<OrderBookLevel> parseOrderBookLevels(const nlohmann::json& levels);
std::vector<BookLevelUpdate> parseL2Updates(const nlohmann::json& deltas);

} // namespace protocol::clientparse

