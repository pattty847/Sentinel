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
#include <span>
#include <QString>
#include <utility>

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

// O(1) LiveOrderBook Implementation
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

    m_nonZeroBidCount = 0;
    m_nonZeroAskCount = 0;
    m_totalBidVolume = 0.0;
    m_totalAskVolume = 0.0;

    sLog_App(QString("🏗️  O(1) LiveOrderBook initialized for %1 with size %2 (%3 -> %4 @ %5)")
              .arg(QString::fromStdString(m_productId)).arg(size)
              .arg(m_min_price).arg(m_max_price).arg(m_tick_size));
}

void LiveOrderBook::applyUpdates(std::span<const BookLevelUpdate> updates,
                                 std::chrono::system_clock::time_point exchange_timestamp,
                                 std::vector<BookDelta>* outDeltas) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (updates.empty()) {
        return;
    }

    if (outDeltas) {
        outDeltas->clear();
        outDeltas->reserve(updates.size());
    }

    m_lastUpdate = exchange_timestamp;  // Use exchange timestamp, not system time!

    for (const auto& update : updates) {
        applyLevelLocked(update.isBid, update.price, update.quantity, outDeltas);
    }
}

size_t LiveOrderBook::getBidCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroBidCount;
}

size_t LiveOrderBook::getAskCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroAskCount;
}

double LiveOrderBook::getBidVolume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalBidVolume;
}

double LiveOrderBook::getAskVolume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalAskVolume;
}

bool LiveOrderBook::isEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nonZeroBidCount == 0 && m_nonZeroAskCount == 0;
}

void LiveOrderBook::applyLevelLocked(bool isBid,
                                     double price,
                                     double quantity,
                                     std::vector<BookDelta>* outDeltas) {
    if (price < m_min_price || price > m_max_price || m_tick_size <= 0.0) {
        return;
    }

    size_t index = price_to_index(price);
    auto& levels = isBid ? m_bids : m_asks;
    if (index >= levels.size()) {
        return;
    }

    double& slot = levels[index];
    const double previous = slot;
    const double newValue = quantity > 0.0 ? quantity : 0.0;

    if (previous == newValue) {
        return;
    }

    auto& totalVolume = isBid ? m_totalBidVolume : m_totalAskVolume;
    auto& nonZeroLevels = isBid ? m_nonZeroBidCount : m_nonZeroAskCount;

    const bool wasNonZero = previous > 0.0;
    const bool isNonZero = newValue > 0.0;

    if (wasNonZero) {
        totalVolume -= previous;
    }
    if (isNonZero) {
        totalVolume += newValue;
    }

    if (wasNonZero != isNonZero) {
        if (isNonZero) {
            ++nonZeroLevels;
        } else if (nonZeroLevels > 0) {
            --nonZeroLevels;
        }
    }

    slot = newValue;

    if (totalVolume < 0.0) {
        totalVolume = 0.0;
    }

    if (outDeltas) {
        outDeltas->push_back({static_cast<uint32_t>(index), static_cast<float>(newValue), isBid});
    }
}

// 🔥 NEW: DataCache LiveOrderBook Methods
// =============================================================================

void DataCache::initializeLiveOrderBook(const std::string& symbol, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, std::chrono::system_clock::time_point exchange_timestamp) {
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

    // Apply the snapshot levels to the new book structure - Use exchange timestamp
    std::vector<BookLevelUpdate> snapshotUpdates;
    snapshotUpdates.reserve(bids.size() + asks.size());
    for (const auto& level : bids) {
        snapshotUpdates.push_back(BookLevelUpdate{true, level.price, level.size});
    }
    for (const auto& level : asks) {
        snapshotUpdates.push_back(BookLevelUpdate{false, level.price, level.size});
    }

    liveBook.applyUpdates(snapshotUpdates, exchange_timestamp, nullptr);

    sLog_Data(QString("🔥 DataCache: Initialized O(1) LiveOrderBook for %1").arg(QString::fromStdString(symbol)));
}

void DataCache::applyLiveOrderBookUpdates(const std::string& symbol,
                                          std::span<const BookLevelUpdate> updates,
                                          std::chrono::system_clock::time_point exchange_timestamp,
                                          std::vector<BookDelta>& outDeltas) {
    std::unique_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        it->second.applyUpdates(updates, exchange_timestamp, &outDeltas);  // Pass exchange timestamp
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
        book->timestamp = liveBook.getLastUpdate();  // Use exchange timestamp, not system time!

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

// Direct dense access (no conversion)
const LiveOrderBook& DataCache::getDirectLiveOrderBook(const std::string& symbol) const {
    std::shared_lock<std::shared_mutex> lock(m_mxLiveBooks);
    
    auto it = m_liveBooks.find(symbol);
    if (it != m_liveBooks.end()) {
        return it->second;  // Direct reference to dense LiveOrderBook
    }
    
    static LiveOrderBook empty;  // Return empty if not found
    return empty;
} 

// LiveOrderBook: captureDenseNonZero implementation
LiveOrderBook::DenseBookSnapshotView LiveOrderBook::captureDenseNonZero(
    std::vector<std::pair<uint32_t, double>>& bidBuffer,
    std::vector<std::pair<uint32_t, double>>& askBuffer,
    size_t maxPerSide) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    bidBuffer.clear();
    askBuffer.clear();
    bidBuffer.reserve(std::min(maxPerSide, m_bids.size()));
    askBuffer.reserve(std::min(maxPerSide, m_asks.size()));

    // Collect non-zero bids from high to low (best bid downward)
    for (size_t i = m_bids.size(); i-- > 0 && bidBuffer.size() < maxPerSide; ) {
        double qty = m_bids[i];
        if (qty > 0.0) {
            bidBuffer.emplace_back(static_cast<uint32_t>(i), qty);
        }
    }

    // Collect non-zero asks from low to high (best ask upward)
    for (size_t i = 0; i < m_asks.size() && askBuffer.size() < maxPerSide; ++i) {
        double qty = m_asks[i];
        if (qty > 0.0) {
            askBuffer.emplace_back(static_cast<uint32_t>(i), qty);
        }
    }

    DenseBookSnapshotView view;
    view.minPrice = m_min_price;
    view.tickSize = m_tick_size;
    view.timestamp = m_lastUpdate; // exchange timestamp
    view.bidLevels = std::span<const std::pair<uint32_t, double>>(bidBuffer.data(), bidBuffer.size());
    view.askLevels = std::span<const std::pair<uint32_t, double>>(askBuffer.data(), askBuffer.size());
    return view;
}
