#include "DataCache.hpp"
#include <algorithm>
#include <mutex>

// Stub implementation for Phase 1 compilation verification

void DataCache::addTrade(const Trade& t) {
    // Use exclusive lock for writing
    std::unique_lock<std::shared_mutex> lock(m_mxTrades);
    
    // Get or create the ring buffer for this symbol
    auto& ring = m_trades[t.product_id];
    
    // Add trade to ring buffer (automatically handles overflow with circular buffer)
    ring.push_back(t);
}

void DataCache::updateBook(const OrderBook& ob) {
    // Use exclusive lock for writing
    std::unique_lock<std::shared_mutex> lock(m_mxBooks);
    
    // Direct O(1) insertion/update
    m_books[ob.product_id] = ob;
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

OrderBook DataCache::book(const std::string& symbol) const {
    // Use shared lock for reading
    std::shared_lock<std::shared_mutex> lock(m_mxBooks);
    
    auto it = m_books.find(symbol);
    if (it != m_books.end()) {
        // Return a copy for thread safety
        return it->second;
    }
    
    return {}; // Return empty OrderBook if symbol not found
} 