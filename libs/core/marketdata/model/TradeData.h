#ifndef TRADEDATA_H
#define TRADEDATA_H

#include <chrono>
#include <string>
#include <vector>
#include <span>
#include <cstdint>
#include <mutex>
#include <utility>

enum class AggressorSide {
    Buy,
    Sell,
    Unknown
};

struct Trade {
    std::chrono::system_clock::time_point timestamp;
    std::string product_id;
    std::string trade_id;
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
    std::chrono::system_clock::time_point timestamp;

    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
};

// Coinbase: subscribe "level2", receive on "l2_data"; new_quantity is absolute size, 0 = remove level.
struct BookDelta {
    uint32_t idx;
    float qty;
    bool isBid;
};

struct BookLevelUpdate {
    bool isBid;
    double price;
    double quantity;
};

class LiveOrderBook {
public:
    LiveOrderBook() = default;
    explicit LiveOrderBook(const std::string& product_id) : m_productId(product_id) {}

    void initialize(double min_price, double max_price, double tick_size);
    void applyUpdates(std::span<const BookLevelUpdate> updates,
                      std::chrono::system_clock::time_point exchange_timestamp,
                      std::vector<BookDelta>* outDeltas);

    const std::vector<double>& getBids() const { return m_bids; }
    const std::vector<double>& getAsks() const { return m_asks; }
    size_t getBidCount() const;
    size_t getAskCount() const;
    double getBidVolume() const;
    double getAskVolume() const;
    bool isEmpty() const;

    double getMinPrice() const { return m_min_price; }
    double getMaxPrice() const { return m_max_price; }
    double getTickSize() const { return m_tick_size; }

    inline double index_to_price(size_t index) const {
        return m_min_price + (index * m_tick_size);
    }

    void setProductId(const std::string& productId) { m_productId = productId; }
    std::string getProductId() const { return m_productId; }
    std::chrono::system_clock::time_point getLastUpdate() const { return m_lastUpdate; }

    struct DenseBookSnapshotView {
        double minPrice = 0.0;
        double tickSize = 1.0;
        std::chrono::system_clock::time_point timestamp;
        std::span<const std::pair<uint32_t, double>> bidLevels; // (index, quantity)
        std::span<const std::pair<uint32_t, double>> askLevels; // (index, quantity)
    };

    DenseBookSnapshotView captureDenseNonZero(
        std::vector<std::pair<uint32_t, double>>& bidBuffer,
        std::vector<std::pair<uint32_t, double>>& askBuffer,
        size_t maxPerSide) const;

    void accumulateRange(double minPrice,
                         double maxPrice,
                         double rowTickSize,
                         std::vector<double>& rowValues,
                         double* bestBid,
                         double* bestAsk) const;

    // Aggregate bids/asks separately into fixed-size rows (thread-safe).
    void accumulateRangeSplit(double minPrice,
                              double maxPrice,
                              double rowTickSize,
                              std::vector<double>& bidRows,
                              std::vector<double>& askRows,
                              double* bestBid,
                              double* bestAsk) const;

private:
    inline size_t price_to_index(double price) const {
        return static_cast<size_t>((price - m_min_price) / m_tick_size);
    }

    void applyLevelLocked(bool isBid,
                           double price,
                           double quantity,
                           std::vector<BookDelta>* outDeltas);

    std::string m_productId;

    // Vectors for O(1) price level management
    std::vector<double> m_bids;
    std::vector<double> m_asks;

    // Book structure configuration
    double m_min_price = 0.0;
    double m_max_price = 0.0;
    double m_tick_size = 0.0;

    size_t m_nonZeroBidCount = 0;
    size_t m_nonZeroAskCount = 0;
    double m_totalBidVolume = 0.0;
    double m_totalAskVolume = 0.0;

    std::chrono::system_clock::time_point m_lastUpdate;
    mutable std::mutex m_mutex;
};

#include <QMetaType>
Q_DECLARE_METATYPE(Trade)
Q_DECLARE_METATYPE(BookDelta)
Q_DECLARE_METATYPE(std::vector<BookDelta>)
Q_DECLARE_METATYPE(BookLevelUpdate)
Q_DECLARE_METATYPE(std::vector<BookLevelUpdate>)

#endif // TRADEDATA_H 
