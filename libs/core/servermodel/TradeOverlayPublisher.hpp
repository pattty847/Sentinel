#pragma once
#include "ServerDataModel.hpp"
#include "SessionManager.hpp"
#include <nlohmann/json.hpp>
#include "../marketdata/rest/CoinbaseRestClient.hpp"
#include <functional>

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
struct TimeWindow {
    int64_t startMs = 0, endMs = 0;
    bool empty() const { return endMs <= startMs; }
};
// Exact input interval for the requested output; no blanket retained-tape copy.
TimeWindow tradeWindow(const Request& request);
using StopRequested = std::function<bool()>;
using CandleFetcher = std::function<CandleFetchResult(int64_t startSec, int64_t endSec, int limit)>;
// Worker-only REST paging for TPO history preceding the retained tape. Empty tape
// (retainedFromMs == 0) backfills the entire requested historical window.
CandleFetchResult fetchTpoCandles(const Request& request, int64_t retainedFromMs,
                                const CandleFetcher& fetch, const StopRequested& stopped = {});
// Pure worker-side builder. Inputs are timestamp-sorted immutable snapshots.
Result build(const Request& request, const std::vector<ServerDataModel::FootprintTradeSample>& trades,
             const std::vector<OHLCVBar>& candles = {}, int64_t retainedFromMs = 0);
// TPO grid for a session type: the base grid for daily sessions; for W1 (x5)
// and M1 (x10) the same rows at a coarser tick, centred on the base band, so a
// whole week or month fits. Footprint and volume profile keep the base grid.
Grid tpoGridFor(const Grid& base, SessionManager::SessionType session);
// At most two buckets: previous close when crossing a boundary, then forming.
std::vector<int64_t> liveBuckets(int64_t nowMs, int64_t previousMs, int64_t tfMs,
                                 int64_t originMs = 0);
} // namespace trade_overlay
