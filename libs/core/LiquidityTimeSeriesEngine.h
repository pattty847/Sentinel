/*
Sentinel — LiquidityTimeSeriesEngine
Role: Aggregates raw order book data into a multi-resolution, time-sliced data structure.
Inputs/Outputs: Takes OrderBook updates; produces collections of LiquidityTimeSlice for rendering.
Threading: Thread-safe for concurrent reads and writes using a QReadWriteLock.
Performance: Optimized for high-frequency updates with logarithmic time complexity for lookups.
Integration: Used by DataProcessor to process the live data stream for the UnifiedGridRenderer.
Observability: Logs timeframe creation and update events via sLog_Render.
Related: LiquidityTimeSeriesEngine.cpp, DataProcessor.hpp, UnifiedGridRenderer.h, TradeData.h.
Assumptions: Order book updates are processed to update the time-sliced liquidity data.
*/
#pragma once

#include <QObject>
#include <QTimer>
#include <deque>
#include <map>
#include <vector>
#include "marketdata/model/TradeData.h"

/**
 * 🎯 LIQUIDITY TIME SERIES ENGINE
 * 
 * This class implements the core temporal order book analysis:
 * - Captures 100ms order book snapshots
 * - Aggregates them into configurable timeframes (250ms, 500ms, 1s, 2s, 5s, etc.)
 * - Provides anti-spoofing detection via persistence ratio
 * - Supports multiple display modes (average, resting, peak, total liquidity)
 * 
 * Key Features:
 * - Dynamic timeframe management
 * - Real-time "current" slice building
 * - Memory-bounded with automatic cleanup
 * - High-performance data structures for GPU rendering
 */

// Order book snapshot at a specific point in time
struct OrderBookSnapshot {
    int64_t timestamp_ms;
    std::map<double, double> bids;  // price -> size
    std::map<double, double> asks;  // price -> size
    
    // Helper methods
    double getBidLiquidity(double price) const {
        auto it = bids.find(price);
        return it != bids.end() ? it->second : 0.0;
    }
    
    double getAskLiquidity(double price) const {
        auto it = asks.find(price);
        return it != asks.end() ? it->second : 0.0;
    }
};

// PHASE 2.2: Tick-based price indexing for O(1) performance
using Tick = int32_t;  // Price levels as integer ticks for O(1) vector access

// Aggregated liquidity data for one time bucket
struct LiquidityTimeSlice {
    int64_t startTime_ms;
    int64_t endTime_ms;
    int64_t duration_ms;
    
    // PHASE 2.2: Tick-based price range for this slice
    Tick minTick = 0;      // Lowest price tick seen in this slice
    Tick maxTick = 0;      // Highest price tick seen in this slice
    double tickSize = 1.0; // Price increment per tick ($1 default)
    
    // Metrics for each price level during this time slice
    struct PriceLevelMetrics {
        double totalLiquidity = 0.0;        // Sum of all liquidity seen
        double avgLiquidity = 0.0;           // Average liquidity during interval
        double maxLiquidity = 0.0;           // Peak liquidity seen
        double minLiquidity = 0.0;           // Minimum liquidity (could be 0)
        double restingLiquidity = 0.0;       // Liquidity that stayed for full duration
        int snapshotCount = 0;               // How many snapshots included this price
        int64_t firstSeen_ms = 0;            // When this price level first appeared
        int64_t lastSeen_ms = 0;             // When this price level last had liquidity
        
        // PHASE 2.4: Version stamp for O(1) presence detection
        uint32_t lastSeenSeq = 0;            // Global sequence number of last snapshot containing this level
        
        // Anti-spoofing detection
        bool wasConsistent() const {
            return snapshotCount > 2;  // Present for at least 3 snapshots
        }
        
        double persistenceRatio(int64_t slice_duration_ms) const {
            if (slice_duration_ms <= 0 || lastSeen_ms <= firstSeen_ms) return 0.0;
            return static_cast<double>(lastSeen_ms - firstSeen_ms) /
                   static_cast<double>(slice_duration_ms);
        }
    };
    
    // 🚀 PHASE 2.2: O(1) vector storage instead of O(log N) std::map
    std::vector<PriceLevelMetrics> bidMetrics;  // Index = (tick - minTick)
    std::vector<PriceLevelMetrics> askMetrics;  // Index = (tick - minTick)
    
    // PHASE 2.2: Tick-based access methods
    Tick priceToTick(double price) const {
        return static_cast<Tick>(std::round(price / tickSize));
    }
    
    double tickToPrice(Tick tick) const {
        return static_cast<double>(tick) * tickSize;
    }
    
    // O(1) metrics access by price
    const PriceLevelMetrics* getMetrics(double price, bool isBid) const {
        Tick tick = priceToTick(price);
        if (tick < minTick || tick > maxTick) return nullptr;
        
        const auto& metrics = isBid ? bidMetrics : askMetrics;
        size_t index = static_cast<size_t>(tick - minTick);
        return (index < metrics.size()) ? &metrics[index] : nullptr;
    }
    
    // Get display value for rendering (PHASE 2.2: O(1) access)
    double getDisplayValue(double price, bool isBid, int displayMode) const;
};

class LiquidityTimeSeriesEngine : public QObject {
    Q_OBJECT

public:
    enum class LiquidityDisplayMode {
        Average = 0,    // Average liquidity during interval
        Maximum = 1,    // Peak liquidity seen
        Resting = 2,    // Only liquidity that persisted full duration (anti-spoof)
        Total = 3       // Sum of all liquidity seen
    };
    Q_ENUM(LiquidityDisplayMode)

private:
    // Base 100ms snapshots (raw data)
    std::deque<OrderBookSnapshot> m_snapshots;
    
    // Dynamic timeframe configuration
    std::vector<int64_t> m_timeframes = {100, 250, 500, 1000, 2000, 5000, 10000}; // ms
    
    // Aggregated time slices for each timeframe
    std::map<int64_t, std::deque<LiquidityTimeSlice>> m_timeSlices;
    
    // 🚀 TIMEFRAME SUGGESTION TRACKING: Only log when suggestion changes
    mutable int64_t m_lastSuggestedTimeframe = 0;
    
    // PHASE 2.3: Efficient rolling timeframe tracking
    std::map<int64_t, LiquidityTimeSlice> m_currentSlices;
    
    // PHASE 2.3: Track last update timestamps for each timeframe (rolling efficiency)
    std::map<int64_t, int64_t> m_lastUpdateTimestamp;
    
    // PHASE 2.4: Global sequence number for O(1) presence detection
    uint32_t m_globalSequence = 0;
    
    // Configuration
    int64_t m_baseTimeframe_ms = 100;           // Snapshot interval
    size_t m_maxHistorySlices = 5000;           // Keep 5000 slices per timeframe
    double m_priceResolution = 1.0;             // $1 price buckets
    size_t m_depthLimit = 2000;                 // 🚀 PERFORMANCE FIX: Max bids/asks to process
    LiquidityDisplayMode m_displayMode = LiquidityDisplayMode::Average;

public:
    explicit LiquidityTimeSeriesEngine(QObject* parent = nullptr);

    // Core data interface
    void addOrderBookSnapshot(const OrderBook& book);
    void addOrderBookSnapshot(const OrderBook& book, double minPrice, double maxPrice);
    
    // Query interface
    const LiquidityTimeSlice* getTimeSlice(int64_t timeframe_ms, int64_t timestamp_ms) const;
    std::vector<const LiquidityTimeSlice*> getVisibleSlices(int64_t timeframe_ms, 
                                                           int64_t viewStart_ms, 
                                                           int64_t viewEnd_ms) const;
    
    // Timeframe management
    void addTimeframe(int64_t duration_ms);
    void removeTimeframe(int64_t duration_ms);
    std::vector<int64_t> getAvailableTimeframes() const;
    
    // 🚀 OPTIMIZATION: Suggest optimal timeframe based on viewport size
    int64_t suggestTimeframe(int64_t viewStart_ms, int64_t viewEnd_ms, int maxSlices = 4000) const;
    
    // Configuration
    void setDisplayMode(LiquidityDisplayMode mode);
    LiquidityDisplayMode getDisplayMode() const { return m_displayMode; }
    void setPriceResolution(double resolution) { m_priceResolution = resolution; }
    double getPriceResolution() const { return m_priceResolution; }

signals:
    void timeSliceReady(int64_t timeframe_ms, const LiquidityTimeSlice& slice);
    void displayModeChanged(LiquidityDisplayMode mode);

private:
    // Time slice processing
    void updateAllTimeframes(const OrderBookSnapshot& snapshot);
    void updateTimeframe(int64_t timeframe_ms, const OrderBookSnapshot& snapshot);
    void addSnapshotToSlice(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot);
    void updatePriceLevelMetrics(LiquidityTimeSlice::PriceLevelMetrics& metrics, 
                                double liquidity, 
                                int64_t timestamp_ms,
                                const LiquidityTimeSlice& slice);
    void updateDisappearingLevels(LiquidityTimeSlice& slice, const OrderBookSnapshot& snapshot);
    void finalizeLiquiditySlice(LiquidityTimeSlice& slice);
    
    // Timeframe management
    void rebuildTimeframe(int64_t timeframe_ms);
    
    // Cleanup
    void cleanupOldData();
    
    // PHASE 2.2: Tick-based utilities
    Tick priceToTick(double price) const {
        return static_cast<Tick>(std::round(price / m_priceResolution));
    }
    
    double tickToPrice(Tick tick) const {
        return static_cast<double>(tick) * m_priceResolution;
    }
    
    // Legacy compatibility
    double quantizePrice(double price) const;
};