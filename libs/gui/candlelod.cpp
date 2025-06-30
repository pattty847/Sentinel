#include "candlelod.h"
#include "Log.hpp"
#include <QDateTime>
#include <algorithm>
#include <chrono>

static constexpr auto CAT = "CandleLOD";

CandleLOD::CandleLOD() {
    // Initialize tracking arrays
    m_currentCandles.fill(nullptr);
    m_lastCandleTime.fill(0);
    
    LOG_I(CAT, "🕯️ CandleLOD INITIALIZED - Multi-timeframe candle system ready!");
}

void CandleLOD::addTrade(const Trade& trade) {
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    // Update all timeframes with this trade
    for (size_t i = 0; i < NUM_TIMEFRAMES; ++i) {
        updateTimeFrame(static_cast<TimeFrame>(i), trade);
    }
    
    // Debug first few trades
    static int lodTradeCount = 0;
    if (++lodTradeCount <= 3) {
        LOG_D(CAT,
              "🕯️ LOD TRADE #{} Price:{} Size:{} Candles:1m={} 5m={} 15m={}",
              lodTradeCount, trade.price, trade.size,
              getCandleCount(TF_1min), getCandleCount(TF_5min), getCandleCount(TF_15min));
    }
}

void CandleLOD::updateTimeFrame(TimeFrame tf, const Trade& trade) {
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    // Get or create current candle for this timeframe
    OHLC* currentCandle = getCurrentCandle(tf, timestamp_ms);
    if (!currentCandle) {
        LOG_W(CAT, "⚠️ Failed to get current candle for timeframe {}", tf);
        return;
    }
    
    // Incorporate trade into candle
    incorporateTrade(*currentCandle, trade);
}

OHLC* CandleLOD::getCurrentCandle(TimeFrame tf, int64_t tradeTime) {
    int64_t candleStartTime = getCandleStartTime(tf, tradeTime);
    
    // Check if we need a new candle
    if (m_lastCandleTime[tf] != candleStartTime) {
        // Finalize previous candle if it exists
        if (m_lastCandleTime[tf] != 0) {
            finalizePreviousCandle(tf, candleStartTime);
        }
        
        // Create new candle
        OHLC newCandle;
        newCandle.timestamp_ms = candleStartTime;
        newCandle.open = 0.0;  // Will be set by first trade
        newCandle.high = 0.0;
        newCandle.low = 0.0;
        newCandle.close = 0.0;
        newCandle.volume = 0.0;
        newCandle.tradeCount = 0;
        
        m_timeFrameData[tf].push_back(newCandle);
        m_currentCandles[tf] = &m_timeFrameData[tf].back();
        m_lastCandleTime[tf] = candleStartTime;
        
        static int newCandleCount = 0;
        if (++newCandleCount <= 5) {
            LOG_D(CAT, "🕯️ NEW CANDLE #{} Timeframe:{} Start:{} Total candles:{}",
                  newCandleCount, CandleUtils::timeFrameName(tf),
                  QDateTime::fromMSecsSinceEpoch(candleStartTime).toString().toStdString(),
                  getCandleCount(tf));
        }
    }
    
    return m_currentCandles[tf];
}

int64_t CandleLOD::getCandleStartTime(TimeFrame tf, int64_t timestamp) const {
    int64_t interval = TIMEFRAME_INTERVALS[tf];
    return (timestamp / interval) * interval;
}

void CandleLOD::finalizePreviousCandle(TimeFrame tf, int64_t newCandleTime) {
    // Previous candle is already in the vector, just update pointer
    // No additional finalization needed for now
    static int finalizeCount = 0;
    if (++finalizeCount <= 3) {
        LOG_D(CAT, "🕯️ FINALIZED CANDLE #{} Timeframe:{} Moving to new candle at:{}",
              finalizeCount, CandleUtils::timeFrameName(tf),
              QDateTime::fromMSecsSinceEpoch(newCandleTime).toString().toStdString());
    }
}

void CandleLOD::incorporateTrade(OHLC& candle, const Trade& trade) {
    // First trade in candle - initialize OHLC
    if (candle.tradeCount == 0) {
        candle.open = trade.price;
        candle.high = trade.price;
        candle.low = trade.price;
        candle.close = trade.price;
    } else {
        // Update high/low
        candle.high = std::max(candle.high, trade.price);
        candle.low = std::min(candle.low, trade.price);
    }
    
    // Always update close (last trade price)
    candle.close = trade.price;
    
    // Accumulate volume
    candle.volume += trade.size;
    candle.tradeCount++;
    
    // Debug first few trade incorporations
    static int incorporateCount = 0;
    if (++incorporateCount <= 5) {
        LOG_D(CAT, "🕯️ INCORPORATE TRADE #{} Into candle: O={} H={} L={} C={} V={} Count={}",
              incorporateCount, candle.open, candle.high, candle.low, candle.close,
              candle.volume, candle.tradeCount);
    }
}

void CandleLOD::prebakeTimeFrames(const std::vector<Trade>& rawTrades) {
    LOG_I(CAT, "🕯️ PREBAKING TIMEFRAMES from {} trades...", rawTrades.size());
    
    // Clear existing data
    for (auto& timeFrameData : m_timeFrameData) {
        timeFrameData.clear();
    }
    m_currentCandles.fill(nullptr);
    m_lastCandleTime.fill(0);
    
    // Process all trades in chronological order
    for (const auto& trade : rawTrades) {
        addTrade(trade);
    }
    
    LOG_I(CAT,
          "🕯️ PREBAKING COMPLETE: 1m={} 5m={} 15m={} 1h={} 1d={}",
          getCandleCount(TF_1min), getCandleCount(TF_5min), getCandleCount(TF_15min),
          getCandleCount(TF_60min), getCandleCount(TF_Daily));
}

void CandleLOD::cleanupOldCandles(int64_t cutoffTime_ms) {
    int totalRemoved = 0;
    
    for (size_t i = 0; i < CandleLOD::NUM_TIMEFRAMES; ++i) {
        auto& candles = m_timeFrameData[i];
        
        // Remove candles older than cutoff
        auto it = std::remove_if(candles.begin(), candles.end(),
            [cutoffTime_ms](const OHLC& candle) {
                return candle.timestamp_ms < cutoffTime_ms;
            });
        
        int removed = std::distance(it, candles.end());
        totalRemoved += removed;
        candles.erase(it, candles.end());
        
        // Update current candle pointer if it was removed
        if (m_currentCandles[i] && 
            m_currentCandles[i]->timestamp_ms < cutoffTime_ms) {
            m_currentCandles[i] = nullptr;
            m_lastCandleTime[i] = 0;
        }
    }
    
    if (totalRemoved > 0) {
        LOG_D(CAT, "🧹 CLEANED UP {} old candles", totalRemoved);
    }
}

void CandleLOD::printStats() const {
    LOG_I(CAT, "🕯️ CANDLE LOD STATS:");
    for (size_t i = 0; i < NUM_TIMEFRAMES; ++i) {
        TimeFrame tf = static_cast<TimeFrame>(i);
        LOG_I(CAT, "  {}: {} candles", CandleUtils::timeFrameName(tf), getCandleCount(tf));
        
        if (!m_timeFrameData[i].empty()) {
            const auto& firstCandle = m_timeFrameData[i].front();
            const auto& lastCandle = m_timeFrameData[i].back();
            LOG_I(CAT, "    Range: {} to {}",
                  QDateTime::fromMSecsSinceEpoch(firstCandle.timestamp_ms).toString().toStdString(),
                  QDateTime::fromMSecsSinceEpoch(lastCandle.timestamp_ms).toString().toStdString());
        }
    }
}

// 🔥 HELPER FUNCTIONS IMPLEMENTATION
namespace CandleUtils {
    int64_t alignToTimeFrame(int64_t timestamp_ms, CandleLOD::TimeFrame tf) {
        const int64_t intervals[CandleLOD::NUM_TIMEFRAMES] = {
            100,
            500,
            1000,
            60 * 1000,
            5 * 60 * 1000,
            15 * 60 * 1000,
            60 * 60 * 1000,
            24 * 60 * 60 * 1000
        };
        
        int64_t interval = intervals[tf];
        return (timestamp_ms / interval) * interval;
    }
    
    const char* timeFrameName(CandleLOD::TimeFrame tf) {
        switch (tf) {
            case CandleLOD::TF_100ms: return "100ms";
            case CandleLOD::TF_500ms: return "500ms";
            case CandleLOD::TF_1sec:  return "1sec";
            case CandleLOD::TF_1min:  return "1min";
            case CandleLOD::TF_5min:  return "5min";
            case CandleLOD::TF_15min: return "15min";
            case CandleLOD::TF_60min: return "1hour";
            case CandleLOD::TF_Daily: return "1day";
            default: return "unknown";
        }
    }
    
    double calculatePixelsPerCandle(double viewWidth, int64_t timeSpan_ms, CandleLOD::TimeFrame tf) {
        const int64_t intervals[CandleLOD::NUM_TIMEFRAMES] = {
            100,
            500,
            1000,
            60 * 1000,
            5 * 60 * 1000,
            15 * 60 * 1000,
            60 * 60 * 1000,
            24 * 60 * 60 * 1000
        };
        
        int64_t candleInterval = intervals[tf];
        double candlesInView = static_cast<double>(timeSpan_ms) / static_cast<double>(candleInterval);
        
        return candlesInView > 0 ? viewWidth / candlesInView : 0.0;
    }
} 