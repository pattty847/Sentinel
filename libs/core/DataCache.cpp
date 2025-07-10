#include "DataCache.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <mutex>
#include <iostream>
#include <QString>

// Stub implementation for Phase 1 compilation verification

void DataCache::addTrade(const Trade& t) {
    // Use exclusive lock for writing
    std::unique_lock<std::shared_mutex> lock(m_mxTrades);
    
    // Get or create the ring buffer for this symbol
    auto& ring = m_trades[t.product_id];
    
    // Add trade to ring buffer (automatically handles overflow with circular buffer)
    ring.push_back(t);
}



std::vector<Trade> DataCache::recentTrades(const std::string& symbol) const {
    // Use shared lock for reading - allows concurrent reads
    std::shared_lock<std::shared_mutex> lock(m_mxTrades);
    
    auto it = m_trades.find(symbol);
    if (it != m_trades.end()) {
        // Return a snapshot copy for thread safety
        return it->second.snapshot();
    }
    
    return {}; // Return empty vector if symbol not found
}

std::vector<Trade> DataCache::tradesSince(const std::string& symbol, const std::string& lastId) const {
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(m_mxTrades);
    
    auto it = m_trades.find(symbol);
    if (it == m_trades.end()) {
        return {}; // No trades for this symbol
    }
    
    // Get snapshot to work with
    std::vector<Trade> allTrades = it->second.snapshot();
    std::vector<Trade> newTrades;
    
    if (lastId.empty()) {
        // Return all trades if no lastId specified
        return allTrades;
    }
    
    // Find trades after lastId
    bool foundLastTrade = false;
    for (const auto& trade : allTrades) {
        if (foundLastTrade) {
            newTrades.push_back(trade);
        } else if (trade.trade_id == lastId) {
            foundLastTrade = true;
            // Don't include the lastTrade itself, only trades after it
        }
    }
    
    // If we didn't find lastId, return all trades (probably a new session)
    if (!foundLastTrade && !allTrades.empty()) {
        return allTrades;
    }
    
    return newTrades;
}



// 🚀 ULTRA-FAST: FastOrderBook Implementation for Bookmap
// =============================================================================

void DataCache::initializeFastOrderBook(const std::string& symbol, const OrderBook& snapshot) {
    std::unique_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    // Create or get existing fast order book
    auto& fastBook = m_fastBooks[symbol];
    fastBook = FastOrderBook(symbol); // Initialize with product ID
    fastBook.initializeFromSnapshot(snapshot);
    
    sLog_Cache(QString("🚀 DataCache: Initialized FastOrderBook for %1").arg(QString::fromStdString(symbol)));
}

void DataCache::updateFastOrderBook(const std::string& symbol, const std::string& side, double price, double quantity) {
    std::unique_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        // O(1) update using FastOrderBook's optimized method
        bool is_bid = (side == "bid");
        it->second.updateLevel(price, quantity, is_bid);
    } else {
        // Create new fast book if it doesn't exist
        auto& fastBook = m_fastBooks[symbol];
        fastBook = FastOrderBook(symbol);
        bool is_bid = (side == "bid");
        fastBook.updateLevel(price, quantity, is_bid);
    }
}

const FastOrderBook* DataCache::getFastOrderBook(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return &(it->second);
    }
    
    return nullptr; // Return nullptr if not found
}

std::vector<OrderBookLevel> DataCache::getFastBids(const std::string& symbol, size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return it->second.getBids(max_levels);
    }
    
    return {}; // Return empty vector if not found
}

std::vector<OrderBookLevel> DataCache::getFastAsks(const std::string& symbol, size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return it->second.getAsks(max_levels);
    }
    
    return {}; // Return empty vector if not found
}

double DataCache::getFastBestBid(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return it->second.getBestBidPrice();
    }
    
    return 0.0; // Return 0 if not found
}

double DataCache::getFastBestAsk(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return it->second.getBestAskPrice();
    }
    
    return 0.0; // Return 0 if not found
}

double DataCache::getFastSpread(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxFastBooks);
    
    auto it = m_fastBooks.find(symbol);
    if (it != m_fastBooks.end()) {
        return it->second.getSpread();
    }
    
    return 0.0; // Return 0 if not found
}



 