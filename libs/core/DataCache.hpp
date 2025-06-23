#pragma once
// ─────────────────────────────────────────────────────────────
// DataCache – lock-efficient store for trades & order books.
// ─────────────────────────────────────────────────────────────
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include "tradedata.h"

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

private:
    using TradeRing = RingBuffer<Trade, 1000>;

    mutable std::shared_mutex                     m_mxTrades;
    mutable std::shared_mutex                     m_mxBooks;
    std::unordered_map<std::string, TradeRing>    m_trades;
    std::unordered_map<std::string, OrderBook>    m_books;
}; 