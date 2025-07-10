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
    // 🚀 ULTRA-FAST: Convert FastOrderBook to OrderBook format for compatibility
    const FastOrderBook* fastBook = m_cache.getFastOrderBook(symbol);
    if (!fastBook) {
        return {}; // Return empty OrderBook if not found
    }
    
    OrderBook book;
    book.product_id = symbol;
    book.timestamp = std::chrono::system_clock::now();
    book.bids = fastBook->getBids(1000); // Get up to 1000 levels
    book.asks = fastBook->getAsks(1000); // Get up to 1000 levels
    
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



// 🚀 ULTRA-FAST: O(1) order book access for Bookmap-style GPU pipeline
const FastOrderBook* CoinbaseStreamClient::getFastOrderBook(const std::string& symbol) const {
    return m_cache.getFastOrderBook(symbol);
}

std::vector<OrderBookLevel> CoinbaseStreamClient::getFastBids(const std::string& symbol, size_t max_levels) const {
    return m_cache.getFastBids(symbol, max_levels);
}

std::vector<OrderBookLevel> CoinbaseStreamClient::getFastAsks(const std::string& symbol, size_t max_levels) const {
    return m_cache.getFastAsks(symbol, max_levels);
}

double CoinbaseStreamClient::getFastBestBid(const std::string& symbol) const {
    return m_cache.getFastBestBid(symbol);
}

double CoinbaseStreamClient::getFastBestAsk(const std::string& symbol) const {
    return m_cache.getFastBestAsk(symbol);
}

double CoinbaseStreamClient::getFastSpread(const std::string& symbol) const {
    return m_cache.getFastSpread(symbol);
}

 