#include "coinbasestreamclient.h"
#include "Authenticator.hpp"
#include "DataCache.hpp"
#include "MarketDataCore.hpp"
#include <iostream>

// --- Constructor & Destructor ---

CoinbaseStreamClient::CoinbaseStreamClient()
    : m_started(false)
{
    // Lazy initialization - components created on first use
    std::cout << "🏗️  CoinbaseStreamClient facade initialized" << std::endl;
}

CoinbaseStreamClient::~CoinbaseStreamClient() {
    if (m_started) {
        stop();
    }
    std::cout << "🏗️  CoinbaseStreamClient facade destroyed" << std::endl;
}

// --- Public API Methods ---

void CoinbaseStreamClient::start() {
    if (m_started) {
        std::cout << "⚠️  Already started, ignoring duplicate start() call" << std::endl;
        return;
    }
    
    ensureComponentsInitialized();
    
    std::cout << "🚀 Starting MarketDataCore..." << std::endl;
    m_marketDataCore->start();
    m_started = true;
    
    std::cout << "✅ CoinbaseStreamClient facade started successfully!" << std::endl;
}

void CoinbaseStreamClient::subscribe(const std::vector<std::string>& symbols) {
    m_symbols = symbols;
    
    if (!m_started) {
        std::cout << "📝 Symbols saved, will subscribe when start() is called" << std::endl;
        return;
    }
    
    ensureComponentsInitialized();
    
    std::cout << "📡 Subscribing to " << symbols.size() << " symbols via MarketDataCore..." << std::endl;
    // Note: MarketDataCore subscribes on construction and start(), no separate subscribe method needed
}

void CoinbaseStreamClient::stop() {
    if (!m_started) {
        return;
    }
    
    std::cout << "🛑 Stopping MarketDataCore..." << std::endl;
    if (m_marketDataCore) {
        m_marketDataCore->stop();
    }
    m_started = false;
    
    std::cout << "✅ CoinbaseStreamClient facade stopped successfully!" << std::endl;
}

// --- Data Access Methods (Identical API) ---

std::vector<Trade> CoinbaseStreamClient::getRecentTrades(const std::string& symbol) {
    ensureComponentsInitialized();
    return m_dataCache->recentTrades(symbol);
}

std::vector<Trade> CoinbaseStreamClient::getNewTrades(const std::string& symbol, const std::string& lastTradeId) {
    ensureComponentsInitialized();
    return m_dataCache->tradesSince(symbol, lastTradeId);
}

OrderBook CoinbaseStreamClient::getOrderBook(const std::string& symbol) {
    ensureComponentsInitialized();
    return m_dataCache->book(symbol);
}

// --- Private Helper Methods ---

void CoinbaseStreamClient::ensureComponentsInitialized() {
    if (!m_authenticator) {
        std::cout << "🔐 Initializing Authenticator..." << std::endl;
        m_authenticator = std::make_unique<Authenticator>();
    }
    
    if (!m_dataCache) {
        std::cout << "💾 Initializing DataCache..." << std::endl;
        m_dataCache = std::make_unique<DataCache>();
    }
    
    if (!m_marketDataCore) {
        std::cout << "🌐 Initializing MarketDataCore..." << std::endl;
        // MarketDataCore constructor takes products, auth, and cache
        m_marketDataCore = std::make_unique<MarketDataCore>(m_symbols, *m_authenticator, *m_dataCache);
    }
}