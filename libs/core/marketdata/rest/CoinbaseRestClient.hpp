#pragma once
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include "../../servermodel/TimeframeAggregator.hpp"
#include "../auth/Authenticator.hpp"

struct CandleFetchResult {
    bool ok = false;
    std::string error;
    std::vector<OHLCVBar> candles;
};

// Server-owned, bounded cache. A null bucket records a successfully fetched
// empty interval, not an error. All fetches are serialized, including overlapping
// requests from different sessions. Never called on the GUI or render thread.
class CandleHistoryCache {
public:
    using Fetch = std::function<CandleFetchResult(int64_t, int64_t, const std::string&)>;
    using Cancel = std::function<bool()>;
    static constexpr size_t kMaxSeries = 16;
    static constexpr size_t kMaxBucketsPerSeries = 8192;
    CandleFetchResult fetch(const std::string& product, int64_t timeframeSec,
                            int64_t startSec, int64_t endSec, int64_t nowSec,
                            const Fetch& fetcher, const Cancel& cancelled = {});
    static int64_t sourceSeconds(int64_t timeframeSec);

private:
    struct Series {
        uint64_t used = 0;
        std::map<int64_t, std::optional<OHLCVBar>> buckets;
    };
    std::mutex m_mutex;
    uint64_t m_use = 0;
    std::map<std::pair<std::string, int64_t>, Series> m_series;
};

struct ProductMetadataResult {
    bool ok = false;
    std::string error;
    std::string quoteIncrement;
    std::string baseIncrement;
    nlohmann::json metadata;
    std::string sourcePath;
};

class CoinbaseRestClient {
public:
    explicit CoinbaseRestClient(Authenticator& auth,
                                std::string host = "api.coinbase.com",
                                std::string port = "443",
                                std::string sslCaBundle = {},
                                std::chrono::milliseconds requestTimeout = std::chrono::seconds(10));

    CandleFetchResult fetchProductCandles(const std::string& productId,
                                          int64_t startSec,
                                          int64_t endSec,
                                          const std::string& granularity,
                                          int limit) const;

    ProductMetadataResult fetchProductMetadata(const std::string& productId) const;

    // Half-open range; only buckets already closed when the request starts.
    CandleFetchResult fetchClosedCandleHistory(const std::string& productId,
                                               int64_t timeframeSec,
                                               int64_t startSec, int64_t endSec,
                                               const CandleHistoryCache::Cancel& cancelled = {}) const;

    static std::optional<std::string> granularityFromSeconds(int64_t timeframeSec);

private:
    struct JsonResult {
        bool ok = false;
        std::string error;
        nlohmann::json body;
        std::string sourcePath;
    };
    JsonResult requestJson(const std::string& publicPath, const std::string& privatePath,
                           const std::string& query, const char* label) const;
    Authenticator& m_auth;
    std::string m_host;
    std::string m_port;
    std::string m_sslCaBundle;
    // One total budget, including DNS and authenticated/public fallback.
    std::chrono::milliseconds m_requestTimeout;
    mutable CandleHistoryCache m_history;
    // Accessed only inside m_history's serialized fetch callback.
    mutable std::chrono::steady_clock::time_point m_nextHistoryRequest{};
};
