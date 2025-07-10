#pragma once
// ─────────────────────────────────────────────────────────────
// Facade that preserves the v1 public surface while delegating
// to the new SRP components underneath.
// ─────────────────────────────────────────────────────────────
#include "DataCache.hpp"
#include "MarketDataCore.hpp"
#include "Authenticator.hpp"
#include "tradedata.h"
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
    
    // 🚀 ULTRA-FAST: O(1) order book access for Bookmap-style GPU pipeline
    [[nodiscard]] const FastOrderBook* getFastOrderBook(const std::string& symbol) const;
    [[nodiscard]] std::vector<OrderBookLevel> getFastBids(const std::string& symbol, size_t max_levels = 1000) const;
    [[nodiscard]] std::vector<OrderBookLevel> getFastAsks(const std::string& symbol, size_t max_levels = 1000) const;
    [[nodiscard]] double getFastBestBid(const std::string& symbol) const;
    [[nodiscard]] double getFastBestAsk(const std::string& symbol) const;
    [[nodiscard]] double getFastSpread(const std::string& symbol) const;
    


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