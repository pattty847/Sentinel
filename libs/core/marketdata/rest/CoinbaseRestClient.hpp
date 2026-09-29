#pragma once
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include "../../servermodel/TimeframeAggregator.hpp"
#include "../auth/Authenticator.hpp"

struct CandleFetchResult {
    bool ok = false;
    std::string error;
    std::vector<OHLCVBar> candles;
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

    static std::optional<std::string> granularityFromSeconds(int64_t timeframeSec);

private:
    Authenticator& m_auth;
    std::string m_host;
    std::string m_port;
    std::string m_sslCaBundle;
    // One total budget, including DNS and authenticated/public fallback.
    std::chrono::milliseconds m_requestTimeout;
};
