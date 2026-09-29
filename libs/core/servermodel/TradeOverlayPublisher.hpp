#pragma once
#include "ServerDataModel.hpp"
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>

namespace trade_overlay {
constexpr int64_t kRefreshMs = 1000;
constexpr size_t kMaxTrades = 250000;
constexpr int kMaxColumns = 512;
constexpr int kMaxGridWidth = 2048;
constexpr int kMaxRows = 2048;
constexpr size_t kMaxBytes = 8 * 1024 * 1024;
struct Grid {
    int width = 512, rows = 2048;
    double tick = 5, maxPrice = 0;
    double minPrice() const { return maxPrice - rows * tick; }
    bool valid() const;
};
enum class Kind { Live, FootprintHistory, TpoHistory };
struct Request {
    std::string symbol;
    Kind kind = Kind::Live;
    Grid grid;
    int64_t footprintMs = 60000, tpoMs = 900000;
    SessionManager::SessionType session = SessionManager::SessionType::H24;
    int64_t nowMs = 0, endMs = 0, previousMs = 0;
    int count = 128;
};
struct Result {
    Grid grid;
    std::vector<std::string> messages;
    std::string error;
};
// Pure worker-side builder. The caller supplies one bounded immutable trade snapshot.
Result build(const Request& request, const std::vector<ServerDataModel::FootprintTradeSample>& trades);
// At most two buckets: previous close when crossing a boundary, then forming.
std::vector<int64_t> liveBuckets(int64_t nowMs, int64_t previousMs, int64_t tfMs,
                                 int64_t originMs = 0);
} // namespace trade_overlay
