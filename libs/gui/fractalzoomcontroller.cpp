#include "fractalzoomcontroller.h"
#include "SentinelLogging.hpp"

// 🎯 TEMPORAL RESOLUTION MAPPING: Each timeframe has coordinated update intervals
const FractalZoomController::TemporalSettings FractalZoomController::TEMPORAL_SETTINGS[] = {
    // OrderBook   TradePoints  Depth  Description
    {100,         16,          5000,  "Ultra-detailed (100ms)"}, // TF_100ms
    {500,         50,          3000,  "High-frequency (500ms)"}, // TF_500ms  
    {1000,        100,         2000,  "Scalping (1sec)"},        // TF_1sec
    {5000,        500,         1500,  "Short-term (1min)"},      // TF_1min
    {15000,       1000,        1000,  "Medium-term (5min)"},     // TF_5min
    {60000,       5000,        750,   "Pattern analysis (15min)"},// TF_15min
    {300000,      30000,       500,   "Trend analysis (1hr)"},   // TF_60min
    {1800000,     300000,      250,   "Long-term (Daily)"}       // TF_Daily
};

FractalZoomController::FractalZoomController(QObject* parent)
    : QObject(parent)
{
    sLog_Init("🎯 FractalZoomController: Initializing coordinated temporal resolution system");
}

CandleLOD::TimeFrame FractalZoomController::selectOptimalTimeFrameForDensity(
    double pixelsPerCandle, size_t targetCandleCount) const {
    
    // 🚀 ENHANCED DENSITY TARGETING: Consider both pixels per candle AND target candle count
    // This ensures we always have reasonable visual density regardless of zoom level
    
    // Calculate recommended pixels per candle based on target count
    // Assuming 1920px width as baseline, target 300 candles = 6.4 pixels per candle
    double recommendedPixelsPerCandle = 1920.0 / targetCandleCount;
    
    // Weight the actual vs recommended (favor actual for responsiveness)
    double weightedPixelsPerCandle = (pixelsPerCandle * 0.7) + (recommendedPixelsPerCandle * 0.3);
    
    // 🎯 BITCOIN-OPTIMIZED LOD with coordinated thresholds
    if (weightedPixelsPerCandle < 1.0)  return CandleLOD::TF_Daily;    // Very zoomed out
    if (weightedPixelsPerCandle < 2.5)  return CandleLOD::TF_60min;    // Zoomed out
    if (weightedPixelsPerCandle < 5.0)  return CandleLOD::TF_15min;    // Medium zoom
    if (weightedPixelsPerCandle < 10.0) return CandleLOD::TF_5min;     // Zoomed in
    if (weightedPixelsPerCandle < 20.0) return CandleLOD::TF_1min;     // Close view
    if (weightedPixelsPerCandle < 40.0) return CandleLOD::TF_1sec;     // Very close
    if (weightedPixelsPerCandle < 80.0) return CandleLOD::TF_500ms;    // Ultra close
    return CandleLOD::TF_100ms;                                        // Extreme zoom
}

int FractalZoomController::getOptimalOrderBookUpdateInterval(CandleLOD::TimeFrame timeframe) const {
    size_t tfIndex = static_cast<size_t>(timeframe);
    if (tfIndex >= 8) { // NUM_TIMEFRAMES
        return 1000; // Default fallback
    }
    return TEMPORAL_SETTINGS[tfIndex].orderBookInterval_ms;
}

int FractalZoomController::getOptimalTradePointUpdateInterval(CandleLOD::TimeFrame timeframe) const {
    size_t tfIndex = static_cast<size_t>(timeframe);
    if (tfIndex >= 8) { // NUM_TIMEFRAMES
        return 1000; // Default fallback
    }
    return TEMPORAL_SETTINGS[tfIndex].tradePointInterval_ms;
}

size_t FractalZoomController::getOptimalOrderBookDepth(CandleLOD::TimeFrame timeframe) const {
    size_t tfIndex = static_cast<size_t>(timeframe);
    if (tfIndex >= 8) { // NUM_TIMEFRAMES
        return 1000; // Default fallback
    }
    return TEMPORAL_SETTINGS[tfIndex].orderBookDepth;
}

bool FractalZoomController::shouldTriggerAggregation(CandleLOD::TimeFrame currentTF, double zoomLevel) const {
    // 🔥 INTELLIGENT AGGREGATION TRIGGER
    // When zoomed way out, we should aggregate to higher timeframes
    // When zoomed way in, we should use finer timeframes
    
    if (zoomLevel < 0.1 && currentTF < CandleLOD::TF_Daily) {
        return true; // Very zoomed out - should aggregate up
    }
    
    if (zoomLevel > 10.0 && currentTF > CandleLOD::TF_100ms) {
        return true; // Very zoomed in - should use finer resolution
    }
    
    return false;
}

bool FractalZoomController::hasCurrentBuildingCandle(CandleLOD::TimeFrame tf) const {
    // 🕯️ ADAPTIVE "NOW CANDLE" LOGIC
    // Finer timeframes should always show building candle
    // Coarser timeframes only show building candle if significant progress
    
    switch (tf) {
        case CandleLOD::TF_100ms:
        case CandleLOD::TF_500ms:
        case CandleLOD::TF_1sec:
            return true; // Always show building candle for sub-minute timeframes
            
        case CandleLOD::TF_1min:
        case CandleLOD::TF_5min:
            return true; // Show building candle for minute timeframes
            
        case CandleLOD::TF_15min:
        case CandleLOD::TF_60min:
            return false; // Don't show building candle for hour+ timeframes (too slow)
            
        case CandleLOD::TF_Daily:
            return false; // Daily building candle changes too slowly to be useful
    }
    
    return false;
}

double FractalZoomController::calculateZoomLevel(int64_t timeSpan_ms, double viewWidth) const {
    // 🔍 ZOOM LEVEL CALCULATION
    // 1.0 = Normal view (1 minute per 100 pixels)
    // 0.1 = Very zoomed out (10 minutes per 100 pixels) 
    // 10.0 = Very zoomed in (6 seconds per 100 pixels)
    
    if (viewWidth <= 0) return 1.0;
    
    double baselineTimePerPixel = 60000.0 / 100.0; // 1 minute per 100 pixels = 600ms per pixel
    double actualTimePerPixel = static_cast<double>(timeSpan_ms) / viewWidth;
    
    return baselineTimePerPixel / actualTimePerPixel;
}

QString FractalZoomController::getZoomLevelDescription(CandleLOD::TimeFrame tf) const {
    size_t tfIndex = static_cast<size_t>(tf);
    if (tfIndex >= 8) { // NUM_TIMEFRAMES
        return "Unknown timeframe";
    }
    return QString(TEMPORAL_SETTINGS[tfIndex].description);
}

void FractalZoomController::onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame) {
    if (m_currentTimeFrame != newTimeFrame) {
        m_currentTimeFrame = newTimeFrame;
        
        // 🔥 EMIT COORDINATED UPDATES to all data layers
        emit orderBookUpdateIntervalChanged(getOptimalOrderBookUpdateInterval(newTimeFrame));
        emit tradePointUpdateIntervalChanged(getOptimalTradePointUpdateInterval(newTimeFrame));
        emit orderBookDepthChanged(getOptimalOrderBookDepth(newTimeFrame));
        emit timeFrameChanged(newTimeFrame);
        
        sLog_Chart("🎯 FRACTAL ZOOM: TimeFrame changed to " << getZoomLevelDescription(newTimeFrame).toStdString().c_str()
                  << " - OrderBook:" << getOptimalOrderBookUpdateInterval(newTimeFrame) << "ms"
                  << " - TradePoints:" << getOptimalTradePointUpdateInterval(newTimeFrame) << "ms"
                  << " - Depth:" << getOptimalOrderBookDepth(newTimeFrame) << " levels");
    }
}

void FractalZoomController::onZoomLevelChanged(double zoomLevel) {
    m_currentZoomLevel = zoomLevel;
    
    // Check if we need to trigger aggregation
    if (shouldTriggerAggregation(m_currentTimeFrame, zoomLevel)) {
        sLog_Chart("🚀 FRACTAL ZOOM: Aggregation trigger - ZoomLevel:" << zoomLevel 
                  << " CurrentTF:" << getZoomLevelDescription(m_currentTimeFrame).toStdString().c_str());
    }
}

double FractalZoomController::calculatePixelsPerCandle(double viewWidth, int64_t timeSpan_ms, 
                                                      CandleLOD::TimeFrame tf) const {
    // Helper function for LOD calculations
    const int64_t intervals[] = {
        100, 500, 1000, 60*1000, 5*60*1000, 15*60*1000, 60*60*1000, 24*60*60*1000
    };
    
    size_t tfIndex = static_cast<size_t>(tf);
    if (tfIndex >= sizeof(intervals) / sizeof(intervals[0])) {
        return 0.0;
    }
    
    int64_t candleInterval = intervals[tfIndex];
    double candlesInView = static_cast<double>(timeSpan_ms) / static_cast<double>(candleInterval);
    
    return candlesInView > 0 ? viewWidth / candlesInView : 0.0;
} 