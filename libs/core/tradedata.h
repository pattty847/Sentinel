#ifndef TRADEDATA_H
#define TRADEDATA_H

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <mutex>

// An enumeration to represent the side of a trade in a type-safe way
// This is better than using raw strings like "buy" or "sell"
enum class AggressorSide {
    Buy,
    Sell,
    Unknown
};

// A struct to represent a single market trade
// This is our C++ equivalent of a Pydantic model for trade data
struct Trade // match
{
    std::chrono::system_clock::time_point timestamp;
    std::string product_id; // The symbol, e.g., "BTC-USD"
    std::string trade_id;  // Added for deduplication
    AggressorSide side;
    double price;
    double size;
};

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

TODO: add the following fields to our Trade struct:

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

struct OrderBookLevel {
    double price;
    double size;
};

struct OrderBook {
    std::string product_id;
    std::chrono::system_clock::time_point timestamp;

    // Sparse representation for WebSocket message parsing
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
};

/*
Order Book:

Level2 Channel
The level2 channel guarantees delivery of all updates and is the easiest way to keep a snapshot of the order book.

// Request
{
  "type": "subscribe",
  "product_ids": ["ETH-USD", "BTC-USD"],
  "channel": "level2",
  "jwt": "XYZ"
}
Subscribe to the level2 channel to guarantee that messages are delivered and your order book is in sync.
The level2 channel sends a message with fields, type ("snapshot" or "update"), product_id, and updates. The field 
updates is an array of objects of {price_level, new_quantity, event_time, side} to represent the entire order book. 
Theevent_time property is the time of the event as recorded by our trading engine.
The new_quantity property is the updated size at that price level, not a delta. A new_quantity of "0" indicates the 
price level can be removed.

// Example:
{
  "channel": "l2_data",
  "client_id": "",
  "timestamp": "2023-02-09T20:32:50.714964855Z",
  "sequence_num": 0,
  "events": [
    {
      "type": "snapshot",
      "product_id": "BTC-USD",
      "updates": [
        {
          "side": "bid",
          "event_time": "1970-01-01T00:00:00Z",
          "price_level": "21921.73",
          "new_quantity": "0.06317902"
        },
        {
          "side": "bid",
          "event_time": "1970-01-01T00:00:00Z",
          "price_level": "21921.3",
          "new_quantity": "0.02"
        }
      ]
    }
  ]
}
*/

// 🔥 NEW: LiveOrderBook - Stateful Order Book for Professional Visualization (O(1) complexity)
class LiveOrderBook {
public:
    LiveOrderBook() = default;
    explicit LiveOrderBook(const std::string& product_id) : m_productId(product_id) {}

    // Initialize the fixed-size order book
    void initialize(double min_price, double max_price, double tick_size);

    // Apply incremental updates (l2update messages)
    void applyUpdate(const std::string& side, double price, double quantity);

    // Get dense data for heatmap rendering
    const std::vector<double>& getBids() const { return m_bids; }
    const std::vector<double>& getAsks() const { return m_asks; }

    // Statistics
    size_t getBidCount() const;
    size_t getAskCount() const;
    bool isEmpty() const;

    // Configuration Accessors
    double getMinPrice() const { return m_min_price; }
    double getMaxPrice() const { return m_max_price; }
    double getTickSize() const { return m_tick_size; }

    // Helper to convert an index back to a price for consumers
    inline double index_to_price(size_t index) const {
        return m_min_price + (index * m_tick_size);
    }

    // Thread-safe access
    void setProductId(const std::string& productId) { m_productId = productId; }
    std::string getProductId() const { return m_productId; }

private:
    // Helper to convert a price to a vector index
    inline size_t price_to_index(double price) const {
        return static_cast<size_t>((price - m_min_price) / m_tick_size);
    }

    std::string m_productId;

    // 🚀 CORE: Vectors for O(1) price level management
    std::vector<double> m_bids;
    std::vector<double> m_asks;

    // Book structure configuration
    double m_min_price = 0.0;
    double m_max_price = 0.0;
    double m_tick_size = 0.0;

    std::chrono::system_clock::time_point m_lastUpdate;
    mutable std::mutex m_mutex; // Thread safety for concurrent access
};

#endif // TRADEDATA_H 