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



// 🚀 UNIVERSAL: UniversalOrderBook Implementation for any asset
// =============================================================================

void DataCache::initializeOrderBook(const std::string& symbol, const OrderBook& snapshot, 
                                   const UniversalOrderBook::Config& config) {
    std::unique_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    // Create or get existing universal order book
    auto& orderBook = m_orderBooks[symbol];
    orderBook = UniversalOrderBook(symbol, config); // Initialize with product ID and config
    
    // Convert snapshot to vector format for initialization
    std::vector<UniversalOrderBookLevel> bids, asks;
    for (const auto& level : snapshot.bids) {
        bids.emplace_back(level.price, level.size, 0);
    }
    for (const auto& level : snapshot.asks) {
        asks.emplace_back(level.price, level.size, 0);
    }
    
    orderBook.initializeFromSnapshot(bids, asks);
    
    sLog_Cache(QString("🚀 DataCache: Initialized UniversalOrderBook for %1 (precision: %2)")
               .arg(QString::fromStdString(symbol))
               .arg(orderBook.getPrecision()));
}

void DataCache::updateOrderBook(const std::string& symbol, const std::string& side, double price, double quantity) {
    std::unique_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        // Update using UniversalOrderBook's method
        bool is_bid = (side == "bid");
        it->second.updateLevel(price, quantity, is_bid);
    } else {
        // Create new order book if it doesn't exist
        auto& orderBook = m_orderBooks[symbol];
        orderBook = UniversalOrderBook(symbol);
        bool is_bid = (side == "bid");
        orderBook.updateLevel(price, quantity, is_bid);
    }
}

const UniversalOrderBook* DataCache::getOrderBook(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return &(it->second);
    }
    
    return nullptr; // Return nullptr if not found
}

std::vector<OrderBookLevel> DataCache::getBids(const std::string& symbol, size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getBids(max_levels);
    }
    
    return {}; // Return empty vector if not found
}

std::vector<OrderBookLevel> DataCache::getAsks(const std::string& symbol, size_t max_levels) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getAsks(max_levels);
    }
    
    return {}; // Return empty vector if not found
}

double DataCache::getBestBid(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getBestBidPrice();
    }
    
    return 0.0; // Return 0 if not found
}

double DataCache::getBestAsk(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getBestAskPrice();
    }
    
    return 0.0; // Return 0 if not found
}

double DataCache::getSpread(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getSpread();
    }
    
    return 0.0; // Return 0 if not found
}

std::vector<UniversalOrderBook::LiquidityChunk> DataCache::getLiquidityChunks(const std::string& symbol, 
                                                                              double chunk_size, bool is_bid_side) const {
    std::shared_lock<std::shared_mutex> lock(m_mxOrderBooks);
    
    auto it = m_orderBooks.find(symbol);
    if (it != m_orderBooks.end()) {
        return it->second.getLiquidityChunks(chunk_size, is_bid_side);
    }
    
    return {}; // Return empty vector if not found
}



 