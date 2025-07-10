#pragma once

#include <QObject>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <memory>
#include <map>
#include "../core/tradedata.h"
#include "candlelod.h"

/**
 * @brief Test Data Player for Fractal Zoom Testing
 * 
 * Loads generated order book test data and replays it at configurable speeds
 * to test fractal zoom aggregation across multiple timeframes.
 * 
 * ENHANCED: Supports pre-aggregated historical data for fractal zoom testing
 */
class TestDataPlayer : public QObject {
    Q_OBJECT

public:
    explicit TestDataPlayer(QObject* parent = nullptr);
    
    // Load test data from JSON file
    bool loadTestData(const QString& filename);
    
    // 🚀 NEW: Pre-aggregate historical data for fractal zoom testing
    bool preAggregateHistoricalData();
    
    // Playback control
    void startPlayback(int interval_ms = 100);
    void stopPlayback();
    void setPlaybackSpeed(double multiplier = 1.0);  // 1.0 = real-time, 10.0 = 10x speed
    
    // Playback state
    bool isPlaying() const { return m_playbackTimer.isActive(); }
    size_t getCurrentIndex() const { return m_currentIndex; }
    size_t getTotalSnapshots() const { return m_snapshots.size(); }
    double getProgress() const;
    
    // Seek to specific time
    void seekToTime(int64_t timestamp_ms);
    void seekToIndex(size_t index);
    
    // Test data info
    QString getMetadata() const;
    
    // 🚀 NEW: Get pre-aggregated historical data for specific timeframe
    const std::vector<OrderBook>& getHistoricalOrderBooks(CandleLOD::TimeFrame timeframe) const;
    const std::vector<OHLC>& getHistoricalCandles(CandleLOD::TimeFrame timeframe) const;
    
    // 🚀 NEW: Check if historical data is ready for fractal zoom testing
    bool isHistoricalDataReady() const { return m_historicalDataReady; }
    
signals:
    void orderBookUpdated(const OrderBook& book);
    void playbackFinished();
    void progressChanged(double percentage);
    
    // 🚀 NEW: Signal when historical data is ready for fractal zoom testing
    void historicalDataReady();

private slots:
    void onPlaybackTick();

private:
    struct OrderBookSnapshot {
        int64_t timestamp;
        QString symbol;
        std::vector<OrderBookLevel> bids;
        std::vector<OrderBookLevel> asks;
        double mid_price;
        double spread;
    };
    
    // Playback state
    QTimer m_playbackTimer;
    std::vector<OrderBookSnapshot> m_snapshots;
    size_t m_currentIndex = 0;
    
    // Playback configuration
    int m_baseInterval_ms = 100;
    double m_speedMultiplier = 1.0;
    
    // Metadata
    QJsonObject m_metadata;
    
    // 🚀 NEW: Pre-aggregated historical data for fractal zoom testing
    std::map<CandleLOD::TimeFrame, std::vector<OrderBook>> m_historicalOrderBooks;
    std::map<CandleLOD::TimeFrame, std::vector<OHLC>> m_historicalCandles;
    bool m_historicalDataReady = false;
    
    // Helper methods
    OrderBookSnapshot parseSnapshot(const QJsonObject& json);
    OrderBookLevel parseOrderLevel(const QJsonObject& json);
    void updatePlaybackTimer();
    
    // 🚀 NEW: Aggregation helpers
    void aggregateOrderBooksForTimeFrame(CandleLOD::TimeFrame timeframe);
    void aggregateCandlesForTimeFrame(CandleLOD::TimeFrame timeframe);
    int64_t getTimeSliceForTimeFrame(CandleLOD::TimeFrame timeframe) const;
    double quantizePrice(double price, CandleLOD::TimeFrame timeframe) const;
}; 