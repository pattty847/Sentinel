#include "instanttestdata.h"
#include <QDebug>
#include <QDateTime>
#include <cmath>

InstantTestData::InstantTestData(QObject* parent) 
    : QObject(parent)
    , m_realtimeTimer(new QTimer(this))
    , m_rng(std::random_device{}())
{
    connect(m_realtimeTimer, &QTimer::timeout, this, &InstantTestData::generateRealtimeUpdate);
    
    // Initialize order book
    generateOrderBook(m_currentPrice, m_currentBids, m_currentAsks);
}

void InstantTestData::generateFullHistoricalSet(int64_t endTime_ms, int64_t historyDuration_ms) {
    int64_t startTime = endTime_ms - historyDuration_ms;
    m_lastTimestamp = startTime;
    
    qDebug() << "🚀 GENERATING INSTANT TEST DATA SET";
    qDebug() << "📊 Time range:" << QDateTime::fromMSecsSinceEpoch(startTime).toString() 
             << "to" << QDateTime::fromMSecsSinceEpoch(endTime_ms).toString();
    
    auto startGeneration = std::chrono::high_resolution_clock::now();
    
    // Generate data for all timeframes simultaneously
    for (int tf = 0; tf < static_cast<int>(DynamicLOD::TimeFrame::COUNT); ++tf) {
        generateTimeFrameData(static_cast<DynamicLOD::TimeFrame>(tf), startTime, endTime_ms);
    }
    
    auto endGeneration = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endGeneration - startGeneration);
    
    qDebug() << "✅ INSTANT TEST DATA COMPLETE in" << duration.count() << "ms";
    qDebug() << "🎯 Ready for immediate zoom testing from 100ms to daily timeframes!";
}

void InstantTestData::startRealTimeSimulation() {
    m_lastTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // 100ms updates for ultra-responsive testing
    m_realtimeTimer->start(100);
    qDebug() << "🚀 REAL-TIME SIMULATION STARTED - 100ms updates";
}

void InstantTestData::stopRealTimeSimulation() {
    m_realtimeTimer->stop();
    qDebug() << "⏹️ Real-time simulation stopped";
}

void InstantTestData::generateTimeFrameData(DynamicLOD::TimeFrame tf, int64_t startTime, int64_t endTime) {
    int64_t bucketDuration = DynamicLOD::getTimeFrameDuration(tf);
    double basePrice = m_currentPrice;
    
    // Calculate number of buckets for this timeframe
    int64_t totalDuration = endTime - startTime;
    int64_t numBuckets = totalDuration / bucketDuration;
    
    qDebug() << "📊 Generating" << DynamicLOD::getTimeFrameName(tf) << "data:"
             << numBuckets << "buckets of" << bucketDuration << "ms each";
    
    for (int64_t i = 0; i < numBuckets; ++i) {
        int64_t bucketStart = DynamicLOD::alignTimestamp(startTime + i * bucketDuration, tf);
        
        // Generate realistic price for this bucket
        double priceOffset = std::sin(i * 0.01) * 50.0 + 
                           (std::uniform_real_distribution<>(-1.0, 1.0)(m_rng) * 10.0);
        double bucketPrice = basePrice + priceOffset;
        
        // Generate order book for this timestamp
        std::vector<OrderBookLevel> bids, asks;
        generateOrderBook(bucketPrice, bids, asks);
        
        // Emit the order book update
        OrderBookSnapshot snapshot;
        snapshot.timestamp_ms = bucketStart;
        snapshot.bids = std::move(bids);
        snapshot.asks = std::move(asks);
        
        emit orderBookUpdate(snapshot);
        
        // Generate some trades within this bucket
        int numTrades = std::uniform_int_distribution<>(1, 5)(m_rng);
        for (int t = 0; t < numTrades; ++t) {
            int64_t tradeTime = bucketStart + (t * bucketDuration) / numTrades;
            double tradePrice = bucketPrice + std::uniform_real_distribution<>(-2.0, 2.0)(m_rng);
            double tradeSize = std::uniform_real_distribution<>(0.001, 1.0)(m_rng);
            bool isBuy = std::uniform_int_distribution<>(0, 1)(m_rng);
            
            emit tradeExecuted(tradeTime, tradePrice, tradeSize, isBuy);
        }
    }
}

void InstantTestData::generateOrderBook(double centerPrice, std::vector<OrderBookLevel>& bids, 
                                       std::vector<OrderBookLevel>& asks) {
    bids.clear();
    asks.clear();
    
    // Generate 50 bid levels and 50 ask levels
    const int numLevels = 50;
    const double spread = 0.5; // $0.50 spread
    
    for (int i = 0; i < numLevels; ++i) {
        // Bids (below center price)
        double bidPrice = centerPrice - spread - (i * 0.25);
        double bidSize = std::uniform_real_distribution<>(0.1, 10.0)(m_rng) * (1.0 + i * 0.1);
        bids.push_back({bidPrice, bidSize});
        
        // Asks (above center price)
        double askPrice = centerPrice + spread + (i * 0.25);
        double askSize = std::uniform_real_distribution<>(0.1, 10.0)(m_rng) * (1.0 + i * 0.1);
        asks.push_back({askPrice, askSize});
    }
}

void InstantTestData::generateRealtimeUpdate() {
    m_lastTimestamp += 100; // 100ms increment
    
    // Generate new price with realistic movement
    m_currentPrice = generateNextPrice();
    
    // Generate fresh order book
    generateOrderBook(m_currentPrice, m_currentBids, m_currentAsks);
    
    // Emit update
    OrderBookSnapshot snapshot;
    snapshot.timestamp_ms = m_lastTimestamp;
    snapshot.bids = m_currentBids;
    snapshot.asks = m_currentAsks;
    
    emit orderBookUpdate(snapshot);
    
    // Generate random trade
    if (std::uniform_real_distribution<>(0.0, 1.0)(m_rng) < 0.7) { // 70% chance of trade
        double tradePrice = m_currentPrice + std::uniform_real_distribution<>(-1.0, 1.0)(m_rng);
        double tradeSize = std::uniform_real_distribution<>(0.001, 2.0)(m_rng);
        bool isBuy = std::uniform_int_distribution<>(0, 1)(m_rng);
        
        emit tradeExecuted(m_lastTimestamp, tradePrice, tradeSize, isBuy);
    }
}

double InstantTestData::generateNextPrice() {
    // Realistic price movement with mean reversion
    std::normal_distribution<> normal(0.0, m_volatility);
    double priceChange = normal(m_rng) * m_currentPrice;
    
    // Add some trend and mean reversion
    static double trend = 0.0;
    trend += std::uniform_real_distribution<>(-0.1, 0.1)(m_rng);
    trend *= 0.99; // Decay trend
    
    return m_currentPrice + priceChange + trend;
}