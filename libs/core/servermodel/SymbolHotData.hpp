#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
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
    // Server-only raw Coinbase levels for all live-book aggregation. GUI replicas
    // receive bucket totals and never populate these maps.
    std::unordered_map<double, double> rawBids, rawAsks;
    double rawMinPrice = 0.0, rawMaxPrice = 0.0;
    bool rawValid = false; // Accepted upstream snapshot plus every in-band delta since it.
    bool awaitingRawBbo = false; // Metadata is ready; one-sided/crossed raw BBO may recover by delta.
    std::vector<double> bucketBids, bucketAsks;
    std::vector<size_t> bucketBidCounts, bucketAskCounts;
    double lastTradePrice = 0.0;
    // False after a disconnect, sequence gap or malformed L2 until the next
    // snapshot. Consumers must not treat an invalid book as observed liquidity.
    std::atomic<bool> bookValid{false};
    
    // Recent history for immediate client snapshots
    // RingBuffer<TickSnapshot, N_TICKS> recentTicks; // TODO: Define TickSnapshot
    
    // We will store aggregated slices here
    // For now, let's keep it simple: just the LiveOrderBook
    
    explicit SymbolHotData(const std::string& s) : symbol(s), liveBook(s) {}
    void clearAggregatedBook() {
        bookValid = false;
        liveBook.clear();
        bucketBids.clear(); bucketAsks.clear();
        bucketBidCounts.clear(); bucketAskCounts.clear();
    }
    void invalidateLiveBook() {
        clearAggregatedBook();
        rawValid = false;
        awaitingRawBbo = false;
        rawMinPrice = rawMaxPrice = 0.0;
        rawBids.clear(); rawAsks.clear();
    }
};
