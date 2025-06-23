#include "CoinbaseStreamClient.hpp"

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
    // TODO: Implement in Phase 5
    return m_cache.book(symbol);
} 