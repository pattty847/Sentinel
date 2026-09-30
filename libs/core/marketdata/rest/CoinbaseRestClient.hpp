#pragma once
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>
#include "../../servermodel/TimeframeAggregator.hpp"
#include "../auth/Authenticator.hpp"

struct CandleFetchResult {
    bool ok = false;
    std::string error;
    std::vector<OHLCVBar> candles;
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
};
