#ifndef COINBASESTREAMCLIENT_H
#define COINBASESTREAMCLIENT_H

#include <string>
#include <vector>
#include <memory>
#include "tradedata.h"

// Forward declarations to avoid heavy includes
class Authenticator;
class DataCache;
class MarketDataCore;

/**
 * @brief Facade for Coinbase WebSocket streaming client
 * 
 * This class provides a simplified interface to stream market data from Coinbase.
 * It delegates all operations to specialized components while maintaining 
 * 100% API compatibility with existing code.
 */
class CoinbaseStreamClient {
public:
    CoinbaseStreamClient();
    ~CoinbaseStreamClient();

    // Main interface - unchanged from original API
    void start();
    void subscribe(const std::vector<std::string>& symbols);
    void stop();

    // Data access methods - identical signatures
    std::vector<Trade> getRecentTrades(const std::string& symbol);
    std::vector<Trade> getNewTrades(const std::string& symbol, const std::string& lastTradeId);
    OrderBook getOrderBook(const std::string& symbol);

private:
    // Component instances - lazy initialized
    std::unique_ptr<Authenticator> m_authenticator;
    std::unique_ptr<DataCache> m_dataCache;  
    std::unique_ptr<MarketDataCore> m_marketDataCore;
    
    // State tracking
    std::vector<std::string> m_symbols;
    bool m_started;
    
    // Private helpers
    void ensureComponentsInitialized();
};

#endif // COINBASESTREAMCLIENT_H
