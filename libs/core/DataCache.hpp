/*
Sentinel — DataCache
Role: A thread-safe, in-memory cache for real-time trades and live order book state.
Inputs/Outputs: Ingests Trade/OrderBook data; provides access to this data via query methods.
Threading: Fully thread-safe using a std::shared_mutex for concurrent reads and exclusive writes.
Performance: Optimized for frequent concurrent reads via shared locking; O(log n) access by product.
Integration: Written to by MarketDataCore; read by components requiring access to market data.
Observability: No internal logging; diagnostics are the responsibility of its clients.
Related: DataCache.cpp, TradeData.h, MarketDataCore.hpp, CoinbaseStreamClient.hpp.
Assumptions: Manages data for multiple independent products, identified by string IDs.
*/
#pragma once
// ─────────────────────────────────────────────────────────────
// DataCache – lock-efficient store for trades & order books.
// ─────────────────────────────────────────────────────────────
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <memory>
#include "TradeData.h"

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
    void updateBook(const OrderBook& ob);

    [[nodiscard]] std::vector<Trade>   recentTrades(const std::string& s) const;
    [[nodiscard]] std::vector<Trade>   tradesSince(const std::string& s, const std::string& lastId) const;
    [[nodiscard]] OrderBook            book(const std::string& s) const;
    
    // 🔥 NEW: LiveOrderBook methods for stateful order book management - PHASE 1.4: Now use exchange timestamps
    void initializeLiveOrderBook(const std::string& symbol, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks, std::chrono::system_clock::time_point exchange_timestamp);
    void updateLiveOrderBook(const std::string& symbol, const std::string& side, double price, double quantity, std::chrono::system_clock::time_point exchange_timestamp);
    
    // LEGACY: Remove in Phase 2 cleanup - kept for backwards compatibility during transition
    [[nodiscard]] std::shared_ptr<const OrderBook> getLiveOrderBook(const std::string& symbol) const;
    
    // PHASE 2.1: Direct dense access (no conversion)
    [[nodiscard]] const LiveOrderBook& getDirectLiveOrderBook(const std::string& symbol) const;
    

private:
    using TradeRing = RingBuffer<Trade, 1000>;

    mutable std::shared_mutex                     m_mxTrades;
    mutable std::shared_mutex                     m_mxBooks;
    mutable std::shared_mutex                     m_mxLiveBooks; // For stateful order books
    std::unordered_map<std::string, TradeRing>    m_trades;
    std::unordered_map<std::string, OrderBook>    m_books;
    std::unordered_map<std::string, LiveOrderBook> m_liveBooks; // 🔥 NEW: Stateful order books
}; 