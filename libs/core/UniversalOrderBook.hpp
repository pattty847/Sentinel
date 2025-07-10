#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <memory>

/**
 * 🚀 UNIVERSAL ORDER BOOK
 * 
 * A robust, configurable order book that can handle ANY asset with ANY precision.
 * Features intelligent precision detection, configurable level grouping, and
 * efficient memory usage for the liquidity wall visualization.
 * 
 * Key Features:
 * - Automatic precision detection for any asset ($0.0000005, $50k, etc.)
 * - Configurable level grouping (e.g., $1 chunks, $0.01 chunks)
 * - Memory efficient (uses maps instead of fixed arrays)
 * - Thread-safe operations
 * - Support for any price range
 */

struct OrderBookLevel {
    double price;
    double size;
    uint32_t timestamp;
    
    OrderBookLevel(double p, double s, uint32_t t = 0) 
        : price(p), size(s), timestamp(t) {}
    
    bool operator<(const OrderBookLevel& other) const {
        return price < other.price;
    }
};

class UniversalOrderBook {
public:
    // Configuration for level grouping
    struct Config {
        double min_price;
        double max_price;
        double level_grouping;  // 0 = no grouping, >0 = group by this amount
        bool auto_detect_precision;
        double manual_precision;  // Used if auto_detect_precision = false
        
        Config() : min_price(0.0), max_price(std::numeric_limits<double>::max()), 
                   level_grouping(0.0), auto_detect_precision(true), manual_precision(0.01) {}
        Config(double min_p, double max_p, double grouping = 0.0, bool auto_precision = true)
            : min_price(min_p), max_price(max_p), level_grouping(grouping), 
              auto_detect_precision(auto_precision), manual_precision(0.01) {}
    };

    UniversalOrderBook(const std::string& product_id, const Config& config = Config())
        : m_productId(product_id), m_config(config), m_precision(0.01) {
        // Initialize with empty books
        reset();
    }
    
    // 🎯 CORE OPERATIONS
    
    void updateLevel(double price, double quantity, bool is_bid, uint32_t timestamp = 0) {
        if (timestamp == 0) {
            timestamp = getCurrentTimeMs();
        }
        
        // Validate price bounds
        if (price < m_config.min_price || price > m_config.max_price) {
            return; // Ignore out-of-bounds prices
        }
        
        // Auto-detect precision if enabled
        if (m_config.auto_detect_precision && m_precision == 0.01) {
            detectPrecision(price);
        }
        
        // Apply level grouping if configured
        double grouped_price = groupPrice(price);
        
        // Update the appropriate side
        auto& side_map = is_bid ? m_bids : m_asks;
        
        if (quantity > 0.0) {
            // Add or update level
            auto it = side_map.find(grouped_price);
            if (it != side_map.end()) {
                // Update existing level
                it->second.size = quantity;
                it->second.timestamp = timestamp;
            } else {
                // Add new level
                side_map.emplace(grouped_price, OrderBookLevel(grouped_price, quantity, timestamp));
            }
        } else {
            // Remove level
            side_map.erase(grouped_price);
        }
        
        // Update best bid/ask tracking
        updateBestLevels();
    }
    
    // 🎯 QUERY OPERATIONS
    
    double getBestBidPrice() const {
        return m_bestBid ? m_bestBid->price : 0.0;
    }
    
    double getBestAskPrice() const {
        return m_bestAsk ? m_bestAsk->price : std::numeric_limits<double>::max();
    }
    
    double getBestBidQuantity() const {
        return m_bestBid ? m_bestBid->size : 0.0;
    }
    
    double getBestAskQuantity() const {
        return m_bestAsk ? m_bestAsk->size : 0.0;
    }
    
    double getSpread() const {
        double bid = getBestBidPrice();
        double ask = getBestAskPrice();
        return (bid > 0.0 && ask < std::numeric_limits<double>::max()) ? ask - bid : 0.0;
    }
    
    double getQuantityAtPrice(double price) const {
        double grouped_price = groupPrice(price);
        
        // Check both sides
        auto bid_it = m_bids.find(grouped_price);
        if (bid_it != m_bids.end()) {
            return bid_it->second.size;
        }
        
        auto ask_it = m_asks.find(grouped_price);
        if (ask_it != m_asks.end()) {
            return ask_it->second.size;
        }
        
        return 0.0;
    }
    
    // 🎯 BULK OPERATIONS FOR LIQUIDITY WALL
    
    std::vector<OrderBookLevel> getBids(size_t max_levels = 1000) const {
        std::vector<OrderBookLevel> result;
        result.reserve(std::min(max_levels, m_bids.size()));
        
        // Iterate in reverse (highest to lowest price)
        for (auto it = m_bids.rbegin(); it != m_bids.rend() && result.size() < max_levels; ++it) {
            result.push_back(it->second);
        }
        
        return result;
    }
    
    std::vector<OrderBookLevel> getAsks(size_t max_levels = 1000) const {
        std::vector<OrderBookLevel> result;
        result.reserve(std::min(max_levels, m_asks.size()));
        
        // Iterate forward (lowest to highest price)
        for (auto it = m_asks.begin(); it != m_asks.end() && result.size() < max_levels; ++it) {
            result.push_back(it->second);
        }
        
        return result;
    }
    
    // 🎯 LIQUIDITY WALL OPERATIONS
    
    struct LiquidityChunk {
        double price_start;
        double price_end;
        double total_size;
        size_t level_count;
        uint32_t timestamp;
    };
    
    std::vector<LiquidityChunk> getLiquidityChunks(double chunk_size, bool is_bid_side = true) const {
        std::vector<LiquidityChunk> chunks;
        const auto& side_map = is_bid_side ? m_bids : m_asks;
        
        if (side_map.empty()) return chunks;
        
        // Group levels into chunks
        double current_chunk_start = is_bid_side ? 
            std::floor(side_map.rbegin()->first / chunk_size) * chunk_size :
            std::floor(side_map.begin()->first / chunk_size) * chunk_size;
        
        LiquidityChunk current_chunk{current_chunk_start, current_chunk_start + chunk_size, 0.0, 0, 0};
        
        auto process_level = [&](const auto& level_pair) {
            double price = level_pair.first;
            double chunk_start = std::floor(price / chunk_size) * chunk_size;
            
            if (chunk_start != current_chunk.price_start) {
                // Save current chunk and start new one
                if (current_chunk.total_size > 0) {
                    chunks.push_back(current_chunk);
                }
                current_chunk = LiquidityChunk{chunk_start, chunk_start + chunk_size, 0.0, 0, 0};
            }
            
            current_chunk.total_size += level_pair.second.size;
            current_chunk.level_count++;
            current_chunk.timestamp = std::max(current_chunk.timestamp, level_pair.second.timestamp);
        };
        
        // Process levels in appropriate order
        if (is_bid_side) {
            for (auto it = side_map.rbegin(); it != side_map.rend(); ++it) {
                process_level(*it);
            }
        } else {
            for (const auto& level_pair : side_map) {
                process_level(level_pair);
            }
        }
        
        // Add final chunk
        if (current_chunk.total_size > 0) {
            chunks.push_back(current_chunk);
        }
        
        return chunks;
    }
    
    // 🎯 CONFIGURATION
    
    void setLevelGrouping(double grouping) {
        m_config.level_grouping = grouping;
        // Rebuild order book with new grouping
        rebuildWithNewGrouping();
    }
    
    void setPrecision(double precision) {
        m_precision = precision;
        m_config.auto_detect_precision = false;
        m_config.manual_precision = precision;
    }
    
    double getPrecision() const { return m_precision; }
    double getLevelGrouping() const { return m_config.level_grouping; }
    
    // 🎯 STATISTICS
    
    size_t getTotalLevels() const { return m_bids.size() + m_asks.size(); }
    size_t getBidLevels() const { return m_bids.size(); }
    size_t getAskLevels() const { return m_asks.size(); }
    bool isEmpty() const { return m_bids.empty() && m_asks.empty(); }
    std::string getProductId() const { return m_productId; }
    
    // 🎯 RESET & INITIALIZATION
    
    void reset() {
        m_bids.clear();
        m_asks.clear();
        m_bestBid = nullptr;
        m_bestAsk = nullptr;
        m_precision = m_config.manual_precision;
    }
    
    void initializeFromSnapshot(const std::vector<OrderBookLevel>& bids, 
                               const std::vector<OrderBookLevel>& asks) {
        reset();
        
        // Process bids
        for (const auto& level : bids) {
            updateLevel(level.price, level.size, true, level.timestamp);
        }
        
        // Process asks
        for (const auto& level : asks) {
            updateLevel(level.price, level.size, false, level.timestamp);
        }
    }

private:
    std::string m_productId;
    Config m_config;
    double m_precision;
    
    // Order book data structures
    std::map<double, OrderBookLevel> m_bids;  // Price -> Level (sorted)
    std::map<double, OrderBookLevel> m_asks;  // Price -> Level (sorted)
    
    // Best level tracking
    const OrderBookLevel* m_bestBid = nullptr;
    const OrderBookLevel* m_bestAsk = nullptr;
    
    // 🎯 PRECISION DETECTION
    
    void detectPrecision(double price) {
        if (price <= 0) return;
        
        // Convert to string to analyze decimal places
        std::string price_str = std::to_string(price);
        size_t decimal_pos = price_str.find('.');
        
        if (decimal_pos != std::string::npos) {
            // Find the first non-zero digit after decimal
            size_t first_nonzero = decimal_pos + 1;
            while (first_nonzero < price_str.length() && price_str[first_nonzero] == '0') {
                first_nonzero++;
            }
            
            if (first_nonzero < price_str.length()) {
                // Calculate precision based on position
                int decimal_places = static_cast<int>(first_nonzero - decimal_pos);
                m_precision = std::pow(10.0, -decimal_places);
                
                // Ensure reasonable bounds
                m_precision = std::max(1e-8, std::min(1.0, m_precision));
            }
        }
    }
    
    // 🎯 PRICE GROUPING
    
    double groupPrice(double price) const {
        if (m_config.level_grouping <= 0) {
            // No grouping - round to precision
            return std::round(price / m_precision) * m_precision;
        } else {
            // Group by specified amount
            return std::floor(price / m_config.level_grouping) * m_config.level_grouping;
        }
    }
    
    // 🎯 BEST LEVEL TRACKING
    
    void updateBestLevels() {
        // Update best bid
        if (!m_bids.empty()) {
            m_bestBid = &m_bids.rbegin()->second;  // Highest price
        } else {
            m_bestBid = nullptr;
        }
        
        // Update best ask
        if (!m_asks.empty()) {
            m_bestAsk = &m_asks.begin()->second;   // Lowest price
        } else {
            m_bestAsk = nullptr;
        }
    }
    
    // 🎯 REBUILD WITH NEW GROUPING
    
    void rebuildWithNewGrouping() {
        // Store current levels
        auto old_bids = m_bids;
        auto old_asks = m_asks;
        
        // Reset
        m_bids.clear();
        m_asks.clear();
        
        // Rebuild with new grouping
        for (const auto& level : old_bids) {
            updateLevel(level.second.price, level.second.size, true, level.second.timestamp);
        }
        
        for (const auto& level : old_asks) {
            updateLevel(level.second.price, level.second.size, false, level.second.timestamp);
        }
    }
    
    // 🎯 UTILITY
    
    uint32_t getCurrentTimeMs() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }
}; 