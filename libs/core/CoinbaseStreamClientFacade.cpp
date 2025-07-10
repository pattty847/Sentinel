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
    // 🚀 UNIVERSAL: Convert UniversalOrderBook to OrderBook format for compatibility
    const UniversalOrderBook* orderBook = m_cache.getOrderBook(symbol);
    if (!orderBook) {
        return {}; // Return empty OrderBook if not found
    }
    
    OrderBook book;
    book.product_id = symbol;
    book.timestamp = std::chrono::system_clock::now();
    book.bids = m_cache.getBids(symbol, 1000); // Get up to 1000 levels
    book.asks = m_cache.getAsks(symbol, 1000); // Get up to 1000 levels
    
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



// 🚀 UNIVERSAL: Universal order book access for any asset
const UniversalOrderBook* CoinbaseStreamClient::getUniversalOrderBook(const std::string& symbol) const {
    return m_cache.getOrderBook(symbol);
}

std::vector<OrderBookLevel> CoinbaseStreamClient::getBids(const std::string& symbol, size_t max_levels) const {
    return m_cache.getBids(symbol, max_levels);
}

std::vector<OrderBookLevel> CoinbaseStreamClient::getAsks(const std::string& symbol, size_t max_levels) const {
    return m_cache.getAsks(symbol, max_levels);
}

double CoinbaseStreamClient::getBestBid(const std::string& symbol) const {
    return m_cache.getBestBid(symbol);
}

double CoinbaseStreamClient::getBestAsk(const std::string& symbol) const {
    return m_cache.getBestAsk(symbol);
}

double CoinbaseStreamClient::getSpread(const std::string& symbol) const {
    return m_cache.getSpread(symbol);
}

 