#include "tradedata.h"
#include "SentinelLogging.hpp"
#include <algorithm>

// Bulk operations for GPU pipeline
std::vector<OrderBookLevel> FastOrderBook::getBids(size_t max_levels) const {
    std::vector<OrderBookLevel> bids;
    bids.reserve(std::min(max_levels, m_totalLevels));
    
    size_t count = 0;
    // Iterate from highest price (best bid) downward
    for (size_t i = m_bestBidIdx; i > 0 && count < max_levels; --i) {
        if (m_levels[i].is_active()) {
            bids.emplace_back(OrderBookLevel{
                indexToPrice(i), 
                static_cast<double>(m_levels[i].quantity)
            });
            count++;
        }
    }
    
    return bids;
}

std::vector<OrderBookLevel> FastOrderBook::getAsks(size_t max_levels) const {
    std::vector<OrderBookLevel> asks;
    asks.reserve(std::min(max_levels, m_totalLevels));
    
    size_t count = 0;
    // Iterate from lowest price (best ask) upward
    for (size_t i = m_bestAskIdx; i < TOTAL_LEVELS && count < max_levels; ++i) {
        if (m_levels[i].is_active()) {
            asks.emplace_back(OrderBookLevel{
                indexToPrice(i), 
                static_cast<double>(m_levels[i].quantity)
            });
            count++;
        }
    }
    
    return asks;
}

// Initialize from full Coinbase snapshot
void FastOrderBook::initializeFromSnapshot(const OrderBook& snapshot) {
    // Reset all levels first
    reset();
    
    m_productId = snapshot.product_id;
    
    // Process bids (highest to lowest)
    for (const auto& bid : snapshot.bids) {
        updateLevel(bid.price, bid.size, true);
    }
    
    // Process asks (lowest to highest)  
    for (const auto& ask : snapshot.asks) {
        updateLevel(ask.price, ask.size, false);
    }
    
    sLog_Core("🚀 FastOrderBook initialized:"
             << "Product:" << m_productId
             << "Bids:" << snapshot.bids.size()
             << "Asks:" << snapshot.asks.size()
             << "Best Bid:" << getBestBidPrice()
             << "Best Ask:" << getBestAskPrice()
             << "Spread:" << getSpread());
} 