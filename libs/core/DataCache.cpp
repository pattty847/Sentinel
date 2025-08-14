/*
Sentinel — DataCache
Role: Implements the thread-safe storage logic for market trades and order books.
Inputs/Outputs: Implements methods for adding trades and updating order book state.
Threading: Uses std::unique_lock for write operations and std::shared_lock for read operations.
Performance: Manages memory by pruning the trade history to a fixed size per product.
Integration: The concrete implementation of the in-memory data store.
Observability: No internal logging.
Related: DataCache.hpp, TradeData.h.
Assumptions: Assumes order book updates can be applied incrementally to the stored state.
*/
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

// 🔥 NEW: O(1) LiveOrderBook Implementation
// =============================================================================

void LiveOrderBook::initialize(double min_price, double max_price, double tick_size) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_min_price = min_price;
    m_max_price = max_price;
    m_tick_size = tick_size;

    if (m_tick_size <= 0) return; // Avoid division by zero

    size_t size = static_cast<size_t>((max_price - min_price) / tick_size) + 1;

    m_bids.resize(size, 0.0);
    m_asks.resize(size, 0.0);

    sLog_App(QString("🏗️  O(1) LiveOrderBook initialized for %1 with size %2 (%3 -> %4 @ %5)")
              .arg(QString::fromStdString(m_productId)).arg(size)
              .arg(m_min_price).arg(m_max_price).arg(m_tick_size));
}

void LiveOrderBook::applyUpdate(const std::string& side, double price, double quantity) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Bounds check
    if (price < m_min_price || price > m_max_price) {
        static int oob_count = 0;
        if (++oob_count % 100 == 1) { // Log every 100th OOB update
            sLog_Data(QString("⚠️ Price %1 is out of configured book bounds [%2, %3] for %4. Ignoring update. [Hit #%5]")
                        .arg(price).arg(m_min_price).arg(m_max_price).arg(QString::fromStdString(m_productId)).arg(oob_count));
        }
        return;
    }

    size_t index = price_to_index(price);
    m_lastUpdate = std::chrono::system_clock::now();

    if (side == "bid") {
        if (index < m_bids.size()) {
            m_bids[index] = quantity;
        }
    } else if (side == "offer" || side == "ask") {
        if (index < m_asks.size()) {
            m_asks[index] = quantity;
        }
    }
}

size_t LiveOrderBook::getBidCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Count non-zero elements
    return std::count_if(m_bids.begin(), m_bids.end(), [](double q){ return q > 0.0; });
}

size_t LiveOrderBook::getAskCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Count non-zero elements
    return std::count_if(m_asks.begin(), m_asks.end(), [](double q){ return q > 0.0; });
}

bool LiveOrderBook::isEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Check if all elements are zero
    return std::all_of(m_bids.begin(), m_bids.end(), [](double q){ return q == 0.0; }) &&
           std::all_of(m_asks.begin(), m_asks.end(), [](double q){ return q == 0.0; });
}

// 🔥 NEW: DataCache LiveOrderBook Methods
// =============================================================================

void DataCache::initializeLiveOrderBook(const std::string& symbol, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks) {
    std::unique_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    // Create or get existing live order book
    auto& liveBook = m_liveBooks[symbol];
    liveBook.setProductId(symbol);

    // TODO: dynamically set the price range based on the current price
    // Use reasonable price ranges to avoid massive memory waste
    if (symbol == "BTC-USD") {
        // BTC: ±$25k around $100k = [75k, 125k] = 5M entries instead of 20M
        liveBook.initialize(75000.0, 125000.0, 0.01);
    } else {
        // Default reasonable range for other crypto pairs
        liveBook.initialize(75000.0, 125000.0, 0.01);
    }

    // Apply the snapshot levels to the new book structure
    for (const auto& level : bids) {
        liveBook.applyUpdate("bid", level.price, level.size);
    }
    for (const auto& level : asks) {
        liveBook.applyUpdate("ask", level.price, level.size);
    }
    
    sLog_Data(QString("🔥 DataCache: Initialized O(1) LiveOrderBook for %1").arg(QString::fromStdString(symbol)));
}

void DataCache::updateLiveOrderBook(const std::string& symbol, const std::string& side, double price, double quantity) {
    std::unique_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        it->second.applyUpdate(side, price, quantity);
    } else {
        // If book doesn't exist, we can't initialize it without a snapshot.
        // The first message for a product MUST be a snapshot.
        static int missing_count = 0;
        if (++missing_count % 100 == 1) { // Log every 100th time
             sLog_Data(QString("⚠️ Dropping update for uninitialized live book '%1'. Waiting for snapshot. [Hit #%2]")
                        .arg(QString::fromStdString(symbol)).arg(missing_count));
        }
    }
}

std::shared_ptr<const OrderBook> DataCache::getLiveOrderBook(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        const auto& liveBook = it->second;
        auto book = std::make_shared<OrderBook>();
        book->product_id = liveBook.getProductId();
        book->timestamp = std::chrono::system_clock::now();

        // Convert dense LiveOrderBook to sparse OrderBook format
        const auto& denseBids = liveBook.getBids();
        const auto& denseAsks = liveBook.getAsks();
        
        // Convert bids (scan from highest to lowest price)
        for (size_t i = denseBids.size(); i-- > 0; ) {
            if (denseBids[i] > 0.0) {
                double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
                book->bids.push_back(OrderBookLevel{price, denseBids[i]});
            }
        }
        
        // Convert asks (scan from lowest to highest price)
        for (size_t i = 0; i < denseAsks.size(); ++i) {
            if (denseAsks[i] > 0.0) {
                double price = liveBook.getMinPrice() + (static_cast<double>(i) * liveBook.getTickSize());
                book->asks.push_back(OrderBookLevel{price, denseAsks[i]});
            }
        }
        
        return book;
    }
    
    return nullptr; // Return nullptr if not found
} 