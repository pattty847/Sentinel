#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include "DataCache.hpp"
#include "SentinelLogging.hpp"

// ----------------------------------------------------------------------------
// CoinbaseMessageParser
// Parses raw JSON strings from the Coinbase WebSocket and updates the
// DataCache with Trade and OrderBook data.
// ----------------------------------------------------------------------------
class CoinbaseMessageParser {
public:
    explicit CoinbaseMessageParser(DataCache& cache);
    void parseAndDispatch(const std::string& payload);
private:
    void dispatch(const nlohmann::json& message);
    DataCache& m_cache;
};
