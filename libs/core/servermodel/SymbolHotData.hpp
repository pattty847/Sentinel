#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "../marketdata/model/TradeData.h"

template <typename T, std::size_t MaxN>
class ServerRingBuffer {
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
    
private:
    std::vector<T> m_data;
    std::size_t    m_head{0};
};

struct SymbolHotData {
    std::string symbol;
    LiveOrderBook liveBook;
    double lastTradePrice = 0.0;
    
    // Recent history for immediate client snapshots
    // RingBuffer<TickSnapshot, N_TICKS> recentTicks; // TODO: Define TickSnapshot
    
    // We will store aggregated slices here
    // For now, let's keep it simple: just the LiveOrderBook
    
    explicit SymbolHotData(const std::string& s) : symbol(s), liveBook(s) {
        liveBook.initialize(0.0, 1000000.0, 0.01); // Default large range
    }
};

