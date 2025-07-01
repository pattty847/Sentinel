#pragma once

#include <vector>
#include <array>
#include <chrono>
#include "tradedata.h"

// 🕯️ CANDLESTICK LOD SYSTEM: Optimize rendering based on zoom level
// Different timeframes pre-baked for efficient GPU rendering

struct OHLC {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    int64_t timestamp_ms = 0;
    int tradeCount = 0;        // Number of trades in this candle
    
    // 🎨 CANDLE APPEARANCE: Derived properties for GPU rendering
    bool isBullish() const { return close > open; }
    double bodyHeight() const { return std::abs(close - open); }
    double wickTop() const { return high - std::max(open, close); }
    double wickBottom() const { return std::min(open, close) - low; }
    
    // 🚀 VOLUME SCALING: Scale candle width by volume
    float volumeScale() const { 
        return std::min(2.0f, static_cast<float>(volume / 1000.0)); // Cap at 2x width
    }
};

class CandleLOD {
public:
    enum TimeFrame {
        TF_100ms = 0,
        TF_500ms = 1,
        TF_1sec  = 2,
        TF_1min  = 3,
        TF_5min  = 4,
        TF_15min = 5,
        TF_60min = 6,
        TF_Daily = 7
    };

    static constexpr size_t NUM_TIMEFRAMES = 8;
    
    CandleLOD();
    
    // 🔥 MAIN API: Process incoming trades and update all timeframes
    void addTrade(const Trade& trade);
    void prebakeTimeFrames(const std::vector<Trade>& rawTrades);
    
    // 🎯 LOD SELECTION: Choose optimal timeframe based on visible pixels per candle
    TimeFrame selectOptimalTimeFrame(double pixelsPerCandle) const {
        if (pixelsPerCandle < 2.0)  return TF_Daily;    // Very zoomed out
        if (pixelsPerCandle < 5.0)  return TF_60min;    // Zoomed out
        if (pixelsPerCandle < 10.0) return TF_15min;    // Medium zoom
        if (pixelsPerCandle < 20.0) return TF_5min;     // Zoomed in
        if (pixelsPerCandle < 40.0) return TF_1min;     // Close view
        if (pixelsPerCandle < 80.0) return TF_1sec;     // Very close
        if (pixelsPerCandle < 160.0) return TF_500ms;   // Ultra close
        return TF_100ms;                                // Extreme zoom
    }
    
    // 🔥 DATA ACCESS: Get pre-baked candles for specific timeframe
    const std::vector<OHLC>& getCandlesForTimeFrame(TimeFrame tf) const {
        return m_timeFrameData[tf];
    }
    
    // 🎯 STATS: Get candle count for performance monitoring
    size_t getCandleCount(TimeFrame tf) const {
        return m_timeFrameData[tf].size();
    }
    
    // 🧹 CLEANUP: Remove old candles to maintain performance
    void cleanupOldCandles(int64_t cutoffTime_ms);
    
    // 🔍 DEBUG: Get info about current state
    void printStats() const;

private:
    // 🕯️ PRE-BAKED TIMEFRAME DATA: multiple timeframes for LOD
    std::array<std::vector<OHLC>, NUM_TIMEFRAMES> m_timeFrameData;
    
    // 🎯 TIMEFRAME INTERVALS: Duration in milliseconds
    static constexpr int64_t TIMEFRAME_INTERVALS[NUM_TIMEFRAMES] = {
        100,            // 100ms
        500,            // 500ms
        1000,           // 1 second
        60 * 1000,      // 1 minute
        5 * 60 * 1000,  // 5 minutes
        15 * 60 * 1000, // 15 minutes
        60 * 60 * 1000, // 1 hour
        24 * 60 * 60 * 1000 // 1 day
    };
    
    // 🔥 CURRENT CANDLE TRACKING: For real-time updates
    std::array<OHLC*, NUM_TIMEFRAMES> m_currentCandles; // Pointer to current candle being built
    std::array<int64_t, NUM_TIMEFRAMES> m_lastCandleTime; // Last candle start time for each timeframe
    
    // Helper methods
    void updateTimeFrame(TimeFrame tf, const Trade& trade);
    OHLC* getCurrentCandle(TimeFrame tf, int64_t tradeTime);
    int64_t getCandleStartTime(TimeFrame tf, int64_t timestamp) const;
    void finalizePreviousCandle(TimeFrame tf, int64_t newCandleTime);
    
    // 🎯 TRADE AGGREGATION: Convert individual trades to OHLC
    void incorporateTrade(OHLC& candle, const Trade& trade);
};

// 🔥 HELPER FUNCTIONS: Time utilities for candle boundaries
namespace CandleUtils {
    // Round timestamp down to candle boundary
    int64_t alignToTimeFrame(int64_t timestamp_ms, CandleLOD::TimeFrame tf);
    
    // Get human-readable timeframe name
    const char* timeFrameName(CandleLOD::TimeFrame tf);
    
    // Calculate pixels per candle for LOD selection
    double calculatePixelsPerCandle(double viewWidth, int64_t timeSpan_ms, CandleLOD::TimeFrame tf);
} 