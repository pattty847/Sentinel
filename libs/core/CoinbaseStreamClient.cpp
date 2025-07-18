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
    OrderBook book = m_cache.getLiveOrderBook(symbol);
    
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

std::vector<OrderBookLevel> CoinbaseStreamClient::getLiveBids(const std::string& symbol) const {
    // 🔥 NEW: Return complete bid levels for dense heatmap visualization
    return m_cache.getLiveBids(symbol);
}

std::vector<OrderBookLevel> CoinbaseStreamClient::getLiveAsks(const std::string& symbol) const {
    // 🔥 NEW: Return complete ask levels for dense heatmap visualization
    return m_cache.getLiveAsks(symbol);
} 