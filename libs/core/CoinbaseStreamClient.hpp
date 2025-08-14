#pragma once
/*
Sentinel — CoinbaseStreamClient
Role: Facade that manages the lifecycle of and simplifies access to core data components.
Inputs/Outputs: Takes symbols to subscribe; provides data via cache or direct MarketDataCore access.
Threading: Methods are called from the main thread; owns MarketDataCore which has a worker thread.
Performance: Simple delegation; performance is determined by the underlying components.
Integration: Instantiated by MainWindowGpu to orchestrate the entire data pipeline.
Observability: Minimal logging; diagnostics are handled by underlying components.
Related: CoinbaseStreamClient.cpp, MarketDataCore.hpp, DataCache.hpp, MainWindowGpu.h.
Assumptions: Assumes exclusive ownership of MarketDataCore, DataCache, and Authenticator.
*/
#include "DataCache.hpp"
#include "MarketDataCore.hpp"
#include "Authenticator.hpp"
#include "TradeData.h"
#include <memory>
#include <vector>
#include <string>

class CoinbaseStreamClient {
public:
    CoinbaseStreamClient();                 // uses default "key.json"
    ~CoinbaseStreamClient();

    void start();
    void stop();
    void subscribe(const std::vector<std::string>& symbols);

    // Public API identical to v1
    [[nodiscard]] std::vector<Trade>   getRecentTrades(const std::string& symbol) const;
    [[nodiscard]] std::vector<Trade>   getNewTrades(const std::string& symbol,
                                                    const std::string& lastTradeId) const;
    [[nodiscard]] OrderBook            getOrderBook(const std::string& symbol) const;
    
    // 🔥 NEW: Dense order book data for professional visualization
    [[nodiscard]] std::shared_ptr<const OrderBook> getLiveOrderBook(const std::string& symbol) const;
    
    // 🔥 NEW: Access to MarketDataCore for real-time signals
    [[nodiscard]] MarketDataCore* getMarketDataCore() const { return m_core.get(); }

    // Non-copyable, non-movable (DataCache contains std::shared_mutex)
    CoinbaseStreamClient(const CoinbaseStreamClient&) = delete;
    CoinbaseStreamClient& operator=(const CoinbaseStreamClient&) = delete;
    CoinbaseStreamClient(CoinbaseStreamClient&&) = delete;
    CoinbaseStreamClient& operator=(CoinbaseStreamClient&&) = delete;

private:
    Authenticator          m_auth;
    DataCache              m_cache;
    std::unique_ptr<MarketDataCore> m_core;   // starts lazily after subscribe()
}; 