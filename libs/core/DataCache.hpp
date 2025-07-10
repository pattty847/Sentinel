#pragma once
// ─────────────────────────────────────────────────────────────
// DataCache – lock-efficient store for trades & order books.
// ─────────────────────────────────────────────────────────────
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include "tradedata.h"
#include "UniversalOrderBook.hpp"

template <typename T, std::size_t MaxN>
class RingBuffer {
public:
    void push_back(T val) {
        if (m_data.size() == MaxN) { 
            m_data[m_head] = std::move(val); 
        } else { 
            m_data.emplace_back(std::move(val)); 
        }
        m_head = (m_head + 1) % MaxN;
    }
    
    [[nodiscard]] std::vector<T> snapshot() const { 
        return m_data; 
    }
    
    [[nodiscard]] std::size_t size() const noexcept { 
        return m_data.size(); 
    }
    
    [[nodiscard]] bool empty() const noexcept { 
        return m_data.empty(); 
    }
    
private:
    std::vector<T> m_data;
    std::size_t    m_head{0};
};

class DataCache {
public:
    void addTrade(const Trade& t);

    [[nodiscard]] std::vector<Trade>   recentTrades(const std::string& s) const;
    [[nodiscard]] std::vector<Trade>   tradesSince(const std::string& s, const std::string& lastId) const;
    
    // 🚀 UNIVERSAL: UniversalOrderBook methods for any asset, any precision
    void initializeOrderBook(const std::string& symbol, const OrderBook& snapshot, 
                            const UniversalOrderBook::Config& config = UniversalOrderBook::Config());
    void updateOrderBook(const std::string& symbol, const std::string& side, double price, double quantity);
    [[nodiscard]] const UniversalOrderBook* getOrderBook(const std::string& symbol) const;
    [[nodiscard]] std::vector<OrderBookLevel> getBids(const std::string& symbol, size_t max_levels = 1000) const;
    [[nodiscard]] std::vector<OrderBookLevel> getAsks(const std::string& symbol, size_t max_levels = 1000) const;
    [[nodiscard]] double getBestBid(const std::string& symbol) const;
    [[nodiscard]] double getBestAsk(const std::string& symbol) const;
    [[nodiscard]] double getSpread(const std::string& symbol) const;
    [[nodiscard]] std::vector<UniversalOrderBook::LiquidityChunk> getLiquidityChunks(const std::string& symbol, 
                                                                                    double chunk_size, bool is_bid_side = true) const;
    


private:
    using TradeRing = RingBuffer<Trade, 1000>;

    mutable std::shared_mutex                     m_mxTrades;
    mutable std::shared_mutex                     m_mxOrderBooks; // For universal order books
    std::unordered_map<std::string, TradeRing>    m_trades;
    std::unordered_map<std::string, UniversalOrderBook> m_orderBooks; // 🚀 Universal order books for any asset
}; 