#ifndef TRADEDATA_H
#define TRADEDATA_H

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <mutex>

/*
Match:
A trade occurred between two orders.
The aggressor or taker order is the one executing immediately after being received and the maker order is a resting order on the book.
The side field indicates the maker order side. If the side is sell this indicates the maker was a sell order and the match is considered 
an up-tick. A buy side match is a down-tick.

{
  "type": "match",
  "trade_id": 10,
  "sequence": 50,
  "maker_order_id": "ac928c66-ca53-498f-9c13-a110027a60e8",
  "taker_order_id": "132fb6ae-456b-4654-b4e0-d681ac05cea1",
  "time": "2014-11-07T08:19:27.028459Z",
  "product_id": "BTC-USD",
  "size": "5.23512",
  "price": "400.23",
  "side": "sell"
}

If authenticated, and you were the taker, the message would also have the following fields:

{
  ...
  "taker_user_id": "5844eceecf7e803e259d0365",
  "user_id": "5844eceecf7e803e259d0365",
  "taker_profile_id": "765d1549-9660-4be2-97d4-fa2d65fa3352",
  "profile_id": "765d1549-9660-4be2-97d4-fa2d65fa3352",
  "taker_fee_rate": "0.005"
}

Similarly, if you were the maker, the message would have the following:

{
  ...
  "maker_user_id": "5f8a07f17b7a102330be40a3",
  "user_id": "5f8a07f17b7a102330be40a3",
  "maker_profile_id": "7aa6b75c-0ff1-11eb-adc1-0242ac120002",
  "profile_id": "7aa6b75c-0ff1-11eb-adc1-0242ac120002",
  "maker_fee_rate": "0.001"
}

*/

// An enumeration to represent the side of a trade in a type-safe way
// This is better than using raw strings like "buy" or "sell"
enum class AggressorSide {
    Buy,
    Sell,
    Unknown
};

// A struct to represent a single market trade
// This is our C++ equivalent of a Pydantic model for trade data
struct Trade
{
    /*
    {
        "type": "match",
        "trade_id": 10,
        "sequence": 50,
        "maker_order_id": "ac928c66-ca53-498f-9c13-a110027a60e8",
        "taker_order_id": "132fb6ae-456b-4654-b4e0-d681ac05cea1",
        "time": "2014-11-07T08:19:27.028459Z",
        "product_id": "BTC-USD",
        "size": "5.23512",
        "price": "400.23",
        "side": "sell"
        }
    */
    std::chrono::system_clock::time_point timestamp;
    std::string product_id; // The symbol, e.g., "BTC-USD"
    std::string trade_id;  // Added for deduplication
    AggressorSide side;
    double price;
    double size;
};

struct OrderBookLevel {
    double price;
    double size;
};

struct OrderBook {
    std::string product_id;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    std::chrono::system_clock::time_point timestamp;
};

// 🔥 NEW: LiveOrderBook - Stateful Order Book for Professional Visualization
class LiveOrderBook {
public:
    LiveOrderBook() = default;
    explicit LiveOrderBook(const std::string& product_id) : m_productId(product_id) {}
    
    // Initialize from snapshot (complete order book state)
    void initializeFromSnapshot(const OrderBook& snapshot);
    
    // Apply incremental updates (l2update messages)
    void applyUpdate(const std::string& side, double price, double quantity);
    
    // Get current complete state as OrderBook for rendering
    OrderBook getCurrentState() const;
    
    // Get dense data for heatmap rendering
    std::vector<OrderBookLevel> getAllBids() const;
    std::vector<OrderBookLevel> getAllAsks() const;
    
    // Statistics
    size_t getBidCount() const;
    size_t getAskCount() const;
    bool isEmpty() const;
    
    // Thread-safe access
    void setProductId(const std::string& productId) { m_productId = productId; }
    std::string getProductId() const { return m_productId; }

private:
    std::string m_productId;
    
    // 🚀 CORE: Sorted maps for O(log N) price level management
    // Key = price, Value = quantity/size
    std::map<double, double> m_bids;  // Higher prices first (reverse order)
    std::map<double, double> m_asks;  // Lower prices first (normal order)
    
    std::chrono::system_clock::time_point m_lastUpdate;
    mutable std::mutex m_mutex; // Thread safety for concurrent access
};

// 🚀 ULTRA-FAST: O(1) Order Book Implementation
// Inspired by Charles Cooper's ITCH implementation (61ns/tick, 16M msg/sec)
// Uses direct array indexing for constant-time operations
class FastOrderBook {
public:
    // Configuration for Coinbase BTC-USD ($0 - $200k range)
    static constexpr double MIN_PRICE = 0.01;     // Minimum meaningful price
    static constexpr double MAX_PRICE = 200000.0; // $200k max
    static constexpr double TICK_SIZE = 0.01;     // 1 cent precision
    // Pre-computed inverse tick for faster price->index conversion (mul vs div)
    static constexpr double INV_TICK = 1.0 / TICK_SIZE;
    static constexpr size_t TOTAL_LEVELS = static_cast<size_t>((MAX_PRICE - MIN_PRICE) / TICK_SIZE) + 1;
    
    // Ultra-compact price level representation (8 bytes total - 50% memory reduction)
    struct PriceLevel {
        float quantity = 0.0f;      // 4 bytes - sufficient precision for quantities
        uint32_t flags = 0;         // 4 bytes - packed: high bit = active, low 31 bits = timestamp/order_count
        
        // Optimized accessors for packed fields
        inline bool is_active() const noexcept { return (flags & 0x80000000U) != 0; }
        inline void set_active(bool active) noexcept { 
            flags = active ? (flags | 0x80000000U) : (flags & 0x7FFFFFFFU); 
        }
        inline uint32_t get_timestamp() const noexcept { return flags & 0x7FFFFFFFU; }
        inline void set_timestamp(uint32_t timestamp) noexcept { 
            flags = (flags & 0x80000000U) | (timestamp & 0x7FFFFFFFU); 
        }
    };
    static_assert(sizeof(PriceLevel) == 8, "PriceLevel must be exactly 8 bytes for cache efficiency");

    FastOrderBook() : m_productId(""), m_bestBidIdx(0), m_bestAskIdx(TOTAL_LEVELS-1) {
        // Pre-allocate arrays - ~160MB for full $200k range (8 bytes × 20M levels)
        m_levels.resize(TOTAL_LEVELS);
        
        // Initialize sentinels for fast best bid/ask tracking
        reset();
    }
    
    explicit FastOrderBook(const std::string& product_id) : FastOrderBook() {
        m_productId = product_id;
    }
    
    // 🚀 O(1) OPERATIONS - The magic happens here!
    
    // New high-performance overload that requires caller-supplied timestamp (avoids syscall per call)
    [[gnu::always_inline]] inline void updateLevel(double price, double quantity, bool is_bid, uint32_t now_ms) {
        size_t idx = priceToIndex(price);
        if (__builtin_expect(idx >= TOTAL_LEVELS, 0)) return; // Bounds check (unlikely)

        PriceLevel& level = m_levels[idx];

        if (quantity > 0.0) {
            // Add/update level
            bool was_empty = !level.is_active();
            level.quantity = static_cast<float>(quantity);
            level.set_active(true);
            level.set_timestamp(now_ms);

            // Update best bid/ask tracking - O(1) with sentinels
            if (is_bid && (was_empty || idx > m_bestBidIdx)) {
                m_bestBidIdx = idx;
            } else if (!is_bid && (was_empty || idx < m_bestAskIdx)) {
                m_bestAskIdx = idx;
            }

            m_totalLevels += was_empty ? 1 : 0;
        } else {
            // Remove level
            if (level.is_active()) {
                level.quantity = 0.0f;
                level.set_active(false);
                m_totalLevels--;

                // Update best bid/ask if we removed the best level
                if (is_bid && idx == m_bestBidIdx) {
                    updateBestBid();
                } else if (!is_bid && idx == m_bestAskIdx) {
                    updateBestAsk();
                }
            }
        }
    }

    // Backwards-compat overload – retains previous signature but pays the timestamp cost.
    inline void updateLevel(double price, double quantity, bool is_bid) {
        updateLevel(price, quantity, is_bid, getCurrentTimeMs());
    }
    
    // O(1) - Get quantity at specific price
    inline double getQuantityAtPrice(double price) const {
        size_t idx = priceToIndex(price);
        return (idx < TOTAL_LEVELS && m_levels[idx].is_active()) ? 
               static_cast<double>(m_levels[idx].quantity) : 0.0;
    }
    
    // O(1) - Get best bid/ask
    inline double getBestBidPrice() const {
        return m_bestBidIdx < TOTAL_LEVELS ? indexToPrice(m_bestBidIdx) : 0.0;
    }
    
    inline double getBestAskPrice() const {
        return m_bestAskIdx < TOTAL_LEVELS ? indexToPrice(m_bestAskIdx) : MAX_PRICE;
    }
    
    inline double getBestBidQuantity() const {
        return m_bestBidIdx < TOTAL_LEVELS ? 
               static_cast<double>(m_levels[m_bestBidIdx].quantity) : 0.0;
    }
    
    inline double getBestAskQuantity() const {
        return m_bestAskIdx < TOTAL_LEVELS ? 
               static_cast<double>(m_levels[m_bestAskIdx].quantity) : 0.0;
    }
    
    // O(1) - Get spread
    inline double getSpread() const {
        return getBestAskPrice() - getBestBidPrice();
    }
    
    // Bulk operations for GPU pipeline
    std::vector<OrderBookLevel> getBids(size_t max_levels = 1000) const;
    std::vector<OrderBookLevel> getAsks(size_t max_levels = 1000) const;
    
    // Statistics - O(1)
    size_t getTotalLevels() const { return m_totalLevels; }
    bool isEmpty() const { return m_totalLevels == 0; }
    std::string getProductId() const { return m_productId; }
    
    // Reset all levels
    void reset() {
        std::fill(m_levels.begin(), m_levels.end(), PriceLevel{});
        m_bestBidIdx = 0;
        m_bestAskIdx = TOTAL_LEVELS - 1;
        m_totalLevels = 0;
    }
    
    // Initialize from full Coinbase snapshot
    void initializeFromSnapshot(const OrderBook& snapshot);

private:
    std::string m_productId;
    std::vector<PriceLevel> m_levels;  // Direct array indexing - the secret sauce!
    
    // O(1) best bid/ask tracking
    size_t m_bestBidIdx;
    size_t m_bestAskIdx;
    size_t m_totalLevels = 0;
    
    // 🚀 CORE: O(1) price <-> index conversion
    [[gnu::always_inline]] inline size_t priceToIndex(double price) const noexcept {
        return static_cast<size_t>((price - MIN_PRICE) * INV_TICK);
    }
    
    inline double indexToPrice(size_t index) const {
        return MIN_PRICE + (static_cast<double>(index) * TICK_SIZE);
    }
    
    // O(n) but only called when best level is removed (rare)
    void updateBestBid() {
        for (size_t i = m_bestBidIdx; i > 0; --i) {
            if (m_levels[i].is_active()) {
                m_bestBidIdx = i;
                return;
            }
        }
        m_bestBidIdx = 0; // No active bids
    }
    
    void updateBestAsk() {
        for (size_t i = m_bestAskIdx; i < TOTAL_LEVELS; ++i) {
            if (m_levels[i].is_active()) {
                m_bestAskIdx = i;
                return;
            }
        }
        m_bestAskIdx = TOTAL_LEVELS - 1; // No active asks
    }
    
    uint32_t getCurrentTimeMs() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
};

#endif // TRADEDATA_H 