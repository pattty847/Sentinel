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
    
    // 🚀 CORE: Sorted maps for efficient price level management
    // Key = price, Value = quantity/size
    std::map<double, double> m_bids;  // Higher prices first (reverse order)
    std::map<double, double> m_asks;  // Lower prices first (normal order)
    
    std::chrono::system_clock::time_point m_lastUpdate;
    mutable std::mutex m_mutex; // Thread safety for concurrent access
};

#endif // TRADEDATA_H 