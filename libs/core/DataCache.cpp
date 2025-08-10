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

// 🔥 NEW: LiveOrderBook Implementation
// =============================================================================

void LiveOrderBook::initializeFromSnapshot(const OrderBook& snapshot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_productId = snapshot.product_id;
    m_lastUpdate = snapshot.timestamp;
    
    // Clear existing state
    m_bids.clear();
    m_asks.clear();
    
    // Initialize bids (higher prices first - reverse sorted for better visualization)
    for (const auto& bid : snapshot.bids) {
        if (bid.size > 0.0) {
            m_bids[bid.price] = bid.size;
        }
    }
    
    // Initialize asks (lower prices first - normal sorted)
    for (const auto& ask : snapshot.asks) {
        if (ask.size > 0.0) {
            m_asks[ask.price] = ask.size;
        }
    }
    
    sLog_App(QString("🏗️  LiveOrderBook initialized for %1 with %2 bids and %3 asks")
              .arg(QString::fromStdString(m_productId)).arg(m_bids.size()).arg(m_asks.size()));
}

void LiveOrderBook::applyUpdate(const std::string& side, double price, double quantity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_lastUpdate = std::chrono::system_clock::now();
    
    if (side == "bid") {
        if (quantity > 0.0) {
            m_bids[price] = quantity; // Add or update bid
        } else {
            m_bids.erase(price); // Remove bid (quantity = 0)
        }
    } else if (side == "offer" || side == "ask") {
        if (quantity > 0.0) {
            m_asks[price] = quantity; // Add or update ask
        } else {
            m_asks.erase(price); // Remove ask (quantity = 0)
        }
    }
}

// In: libs/core/DataCache.cpp

OrderBook LiveOrderBook::getCurrentState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    OrderBook book;
    book.product_id = m_productId;
    book.timestamp = m_lastUpdate;
    
    // Convert bids map to vector
    book.bids.reserve(m_bids.size());
    // 🔥 SORTING FIX: Use reverse iterators (rbegin, rend) to sort bids from highest to lowest price
    for (auto it = m_bids.rbegin(); it != m_bids.rend(); ++it) {
        book.bids.push_back({it->first, it->second});
    }
    
    // Convert asks map to vector (already sorted correctly low-to-high)
    book.asks.reserve(m_asks.size());
    for (const auto& [price, size] : m_asks) {
        book.asks.push_back({price, size});
    }
    
    return book;
}

std::vector<OrderBookLevel> LiveOrderBook::getAllBids() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<OrderBookLevel> bids;
    bids.reserve(m_bids.size());
    
    // Sort bids by price (highest first) for proper visualization
    for (auto it = m_bids.rbegin(); it != m_bids.rend(); ++it) {
        bids.push_back({it->first, it->second});
    }
    
    return bids;
}

std::vector<OrderBookLevel> LiveOrderBook::getAllAsks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<OrderBookLevel> asks;
    asks.reserve(m_asks.size());
    
    // Sort asks by price (lowest first) for proper visualization
    for (const auto& [price, size] : m_asks) {
        asks.push_back({price, size});
    }
    
    return asks;
}

size_t LiveOrderBook::getBidCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bids.size();
}

size_t LiveOrderBook::getAskCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_asks.size();
}

bool LiveOrderBook::isEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bids.empty() && m_asks.empty();
}

// 🔥 NEW: DataCache LiveOrderBook Methods
// =============================================================================

void DataCache::initializeLiveOrderBook(const std::string& symbol, const OrderBook& snapshot) {
    std::unique_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    // Create or get existing live order book
    auto& liveBook = m_liveBooks[symbol];
    liveBook.setProductId(symbol);
    liveBook.initializeFromSnapshot(snapshot);
    
    sLog_Data(QString("🔥 DataCache: Initialized LiveOrderBook for %1").arg(QString::fromStdString(symbol)));
}

void DataCache::updateLiveOrderBook(const std::string& symbol, const std::string& side, double price, double quantity) {
    std::unique_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        it->second.applyUpdate(side, price, quantity);
    } else {
        // Create new live book if it doesn't exist
        auto& liveBook = m_liveBooks[symbol];
        liveBook.setProductId(symbol);
        liveBook.applyUpdate(side, price, quantity);
    }
}

OrderBook DataCache::getLiveOrderBook(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        OrderBook book = it->second.getCurrentState();
        
        // 🔍 DEBUG: Log what we're returning from the cache
        static int cacheDebugCount = 0;
        if (++cacheDebugCount % 30 == 1) { // Log every 30th call
            sLog_Data(QString("🔍 DATACACHE getLiveOrderBook: Found '%1' → returning %2 bids, %3 asks [call #%4]")
                       .arg(QString::fromStdString(symbol)).arg(book.bids.size()).arg(book.asks.size()).arg(cacheDebugCount));
        }
        
        return book;
    }
    
    // 🔍 DEBUG: Log when symbol is not found
    static int notFoundCount = 0;
    if (++notFoundCount % 10 == 1) { // Log every 10th miss
        QString availableSymbols;
        for (const auto& [sym, book] : m_liveBooks) {
            availableSymbols += QString("'%1' ").arg(QString::fromStdString(sym));
        }
        sLog_Data(QString("⚠️ DATACACHE getLiveOrderBook: Symbol '%1' NOT FOUND in live books! Available symbols: %2 [miss #%3]")
                   .arg(QString::fromStdString(symbol)).arg(availableSymbols).arg(notFoundCount));
    }
    
    return {}; // Return empty OrderBook if not found
}

std::vector<OrderBookLevel> DataCache::getLiveBids(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        return it->second.getAllBids();
    }
    
    return {}; // Return empty vector if not found
}

std::vector<OrderBookLevel> DataCache::getLiveAsks(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        return it->second.getAllAsks();
    }
    
    return {}; // Return empty vector if not found
} 