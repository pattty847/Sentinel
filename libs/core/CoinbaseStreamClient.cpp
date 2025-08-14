/*
Sentinel — CoinbaseStreamClient
Role: Implements the facade that delegates calls to the underlying data components.
Inputs/Outputs: Lazily initializes MarketDataCore on subscribe; delegates data queries to DataCache.
Threading: All methods are executed on the caller's thread (typically main).
Performance: All methods are lightweight delegations to the underlying components.
Integration: The concrete implementation of the facade used by the GUI.
Observability: Uses std::cout for debugging in getOrderBook.
Related: CoinbaseStreamClient.hpp, MarketDataCore.hpp, DataCache.hpp.
Assumptions: MarketDataCore is null until the first call to subscribe().
*/
#include "CoinbaseStreamClient.hpp"
#include <iostream>

// New facade implementation - Phase 1 compilation verification stub

CoinbaseStreamClient::CoinbaseStreamClient() = default;

CoinbaseStreamClient::~CoinbaseStreamClient() {
    // TODO: Implement in Phase 5
    stop();
}

void CoinbaseStreamClient::start() {
    // TODO: Implement in Phase 5
    if (m_core) m_core->start();
}

void CoinbaseStreamClient::stop() {
    // TODO: Implement in Phase 5
    if (m_core) m_core->stop();
}

void CoinbaseStreamClient::subscribe(const std::vector<std::string>& symbols) {
    // TODO: Implement in Phase 5
    if (symbols.empty()) return;
    m_core = std::make_unique<MarketDataCore>(symbols, m_auth, m_cache);
    m_core->start();
}

std::vector<Trade> CoinbaseStreamClient::getRecentTrades(const std::string& symbol) const {
    // TODO: Implement in Phase 5
    return m_cache.recentTrades(symbol);
}

std::vector<Trade> CoinbaseStreamClient::getNewTrades(const std::string& symbol,
                                                     const std::string& lastTradeId) const {
    // TODO: Implement in Phase 5
    return m_cache.tradesSince(symbol, lastTradeId);
}

OrderBook CoinbaseStreamClient::getOrderBook(const std::string& symbol) const {
    // 🔥 NEW: Return dense LiveOrderBook data for professional visualization
    auto bookPtr = m_cache.getLiveOrderBook(symbol);
    OrderBook book = bookPtr ? *bookPtr : OrderBook{};
    
    // 🔍 DEBUG: Trace the final connection to identify the broken wire
    static int debugCount = 0;
    if (++debugCount % 20 == 1) { // Log every 20th call
        std::cout << "🔍 FACADE getOrderBook: symbol='" << symbol 
                  << "' → bids=" << book.bids.size() 
                  << " asks=" << book.asks.size() 
                  << " [call #" << debugCount << "]" << std::endl;
    }
    
    return book;
}

std::shared_ptr<const OrderBook> CoinbaseStreamClient::getLiveOrderBook(const std::string& symbol) const {
    return m_cache.getLiveOrderBook(symbol);
}