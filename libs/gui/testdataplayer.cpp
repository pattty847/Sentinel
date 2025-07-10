#include "testdataplayer.h"
#include "SentinelLogging.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDebug>
#include <algorithm>
#include <cmath>

TestDataPlayer::TestDataPlayer(QObject* parent) : QObject(parent) {
    connect(&m_playbackTimer, &QTimer::timeout, this, &TestDataPlayer::onPlaybackTick);
    sLog_Init("🎯 TestDataPlayer: Initialized for fractal zoom testing");
}

bool TestDataPlayer::loadTestData(const QString& filename) {
    sLog_Init("📁 Loading test data from:" << filename.toStdString().c_str());
    
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        sLog_Init("❌ Failed to open test data file:" << filename.toStdString().c_str());
        return false;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        sLog_Init("❌ JSON parse error:" << error.errorString().toStdString().c_str());
        return false;
    }
    
    QJsonObject root = doc.object();
    m_metadata = root["metadata"].toObject();
    QJsonArray snapshots = root["snapshots"].toArray();
    
    sLog_Init("📊 Loading" << snapshots.size() << "snapshots...");
    
    m_snapshots.clear();
    m_snapshots.reserve(snapshots.size());
    
    for (const QJsonValue& value : snapshots) {
        OrderBookSnapshot snapshot = parseSnapshot(value.toObject());
        m_snapshots.push_back(snapshot);
    }
    
    m_currentIndex = 0;
    
    sLog_Init("✅ Loaded" << m_snapshots.size() << "order book snapshots");
    sLog_Init("📋 Test data metadata:" << getMetadata().toStdString().c_str());
    
    // 🚀 NEW: Pre-aggregate historical data for fractal zoom testing (SKIP FOR NOW - TOO EXPENSIVE)
    // if (!preAggregateHistoricalData()) {
    //     sLog_Init("⚠️ Failed to pre-aggregate historical data - fractal zoom testing may be limited");
    // }
    sLog_Init("🚀 SKIPPING EXPENSIVE PRE-AGGREGATION - Using live playback for fractal zoom testing");
    
    return true;
}

// 🚀 NEW: Pre-aggregate historical data for fractal zoom testing
bool TestDataPlayer::preAggregateHistoricalData() {
    sLog_Init("🚀 Pre-aggregating historical data for fractal zoom testing...");
    
    if (m_snapshots.empty()) {
        sLog_Init("❌ No snapshots available for aggregation");
        return false;
    }
    
    // Clear existing historical data
    m_historicalOrderBooks.clear();
    m_historicalCandles.clear();
    
    // Aggregate for each timeframe
    const std::vector<CandleLOD::TimeFrame> timeframes = {
        CandleLOD::TF_100ms, CandleLOD::TF_500ms, CandleLOD::TF_1sec,
        CandleLOD::TF_1min, CandleLOD::TF_5min, CandleLOD::TF_15min,
        CandleLOD::TF_60min, CandleLOD::TF_Daily
    };
    
    for (CandleLOD::TimeFrame tf : timeframes) {
        sLog_Init("📊 Aggregating timeframe" << static_cast<int>(tf) << "...");
        aggregateOrderBooksForTimeFrame(tf);
        aggregateCandlesForTimeFrame(tf);
    }
    
    m_historicalDataReady = true;
    emit historicalDataReady();
    
    sLog_Init("✅ Historical data pre-aggregation complete!");
    sLog_Init("📊 Aggregated data summary:");
    for (const auto& [tf, orderBooks] : m_historicalOrderBooks) {
        sLog_Init("  TimeFrame" << static_cast<int>(tf) << ":" << orderBooks.size() << "order books," 
                 << m_historicalCandles.at(tf).size() << "candles");
    }
    
    return true;
}

void TestDataPlayer::aggregateOrderBooksForTimeFrame(CandleLOD::TimeFrame timeframe) {
    int64_t timeSlice = getTimeSliceForTimeFrame(timeframe);
    std::map<int64_t, OrderBook> timeBuckets;
    
    // Group snapshots by time bucket
    for (const auto& snapshot : m_snapshots) {
        int64_t timeBucket = (snapshot.timestamp / timeSlice) * timeSlice;
        
        if (timeBuckets.find(timeBucket) == timeBuckets.end()) {
            // Create new bucket
            OrderBook book;
            book.bids = snapshot.bids;
            book.asks = snapshot.asks;
            timeBuckets[timeBucket] = book;
        } else {
            // Aggregate with existing bucket (sum volumes at same price levels)
            OrderBook& existingBook = timeBuckets[timeBucket];
            
            // Aggregate bids
            for (const auto& newBid : snapshot.bids) {
                bool found = false;
                for (auto& existingBid : existingBook.bids) {
                    if (std::abs(existingBid.price - newBid.price) < 0.01) {
                        existingBid.size += newBid.size;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    existingBook.bids.push_back(newBid);
                }
            }
            
            // Aggregate asks
            for (const auto& newAsk : snapshot.asks) {
                bool found = false;
                for (auto& existingAsk : existingBook.asks) {
                    if (std::abs(existingAsk.price - newAsk.price) < 0.01) {
                        existingAsk.size += newAsk.size;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    existingBook.asks.push_back(newAsk);
                }
            }
        }
    }
    
    // Convert buckets to vector
    std::vector<OrderBook> aggregatedBooks;
    aggregatedBooks.reserve(timeBuckets.size());
    for (const auto& [timestamp, book] : timeBuckets) {
        aggregatedBooks.push_back(book);
    }
    
    m_historicalOrderBooks[timeframe] = std::move(aggregatedBooks);
}

void TestDataPlayer::aggregateCandlesForTimeFrame(CandleLOD::TimeFrame timeframe) {
    int64_t timeSlice = getTimeSliceForTimeFrame(timeframe);
    std::map<int64_t, OHLC> timeBuckets;
    
    // Group snapshots by time bucket and create OHLC candles
    for (const auto& snapshot : m_snapshots) {
        int64_t timeBucket = (snapshot.timestamp / timeSlice) * timeSlice;
        double midPrice = snapshot.mid_price;
        
        if (timeBuckets.find(timeBucket) == timeBuckets.end()) {
            // Create new candle
            OHLC candle;
            candle.open = midPrice;
            candle.high = midPrice;
            candle.low = midPrice;
            candle.close = midPrice;
            candle.volume = 0.0; // We don't have volume in order book data
            candle.timestamp_ms = timeBucket;
            timeBuckets[timeBucket] = candle;
        } else {
            // Update existing candle
            OHLC& existingCandle = timeBuckets[timeBucket];
            existingCandle.high = std::max(existingCandle.high, midPrice);
            existingCandle.low = std::min(existingCandle.low, midPrice);
            existingCandle.close = midPrice;
        }
    }
    
    // Convert buckets to vector
    std::vector<OHLC> aggregatedCandles;
    aggregatedCandles.reserve(timeBuckets.size());
    for (const auto& [timestamp, candle] : timeBuckets) {
        aggregatedCandles.push_back(candle);
    }
    
    m_historicalCandles[timeframe] = std::move(aggregatedCandles);
}

int64_t TestDataPlayer::getTimeSliceForTimeFrame(CandleLOD::TimeFrame timeframe) const {
    switch (timeframe) {
        case CandleLOD::TF_100ms: return 100;
        case CandleLOD::TF_500ms: return 500;
        case CandleLOD::TF_1sec: return 1000;
        case CandleLOD::TF_1min: return 60000;
        case CandleLOD::TF_5min: return 300000;
        case CandleLOD::TF_15min: return 900000;
        case CandleLOD::TF_60min: return 3600000;
        case CandleLOD::TF_Daily: return 86400000;
        default: return 1000;
    }
}

double TestDataPlayer::quantizePrice(double price, CandleLOD::TimeFrame timeframe) const {
    // Simple price quantization - could be enhanced for more sophisticated bucketing
    double tickSize = 0.01; // 1 cent tick size for BTC
    return std::round(price / tickSize) * tickSize;
}

const std::vector<OrderBook>& TestDataPlayer::getHistoricalOrderBooks(CandleLOD::TimeFrame timeframe) const {
    static const std::vector<OrderBook> empty;
    auto it = m_historicalOrderBooks.find(timeframe);
    return (it != m_historicalOrderBooks.end()) ? it->second : empty;
}

const std::vector<OHLC>& TestDataPlayer::getHistoricalCandles(CandleLOD::TimeFrame timeframe) const {
    static const std::vector<OHLC> empty;
    auto it = m_historicalCandles.find(timeframe);
    return (it != m_historicalCandles.end()) ? it->second : empty;
}

TestDataPlayer::OrderBookSnapshot TestDataPlayer::parseSnapshot(const QJsonObject& json) {
    OrderBookSnapshot snapshot;
    snapshot.timestamp = json["timestamp"].toVariant().toLongLong();
    snapshot.symbol = json["symbol"].toString();
    snapshot.mid_price = json["mid_price"].toDouble();
    snapshot.spread = json["spread"].toDouble();
    
    // Parse bids
    QJsonArray bidsArray = json["bids"].toArray();
    snapshot.bids.reserve(bidsArray.size());
    for (const QJsonValue& bidValue : bidsArray) {
        snapshot.bids.push_back(parseOrderLevel(bidValue.toObject()));
    }
    
    // Parse asks
    QJsonArray asksArray = json["asks"].toArray();
    snapshot.asks.reserve(asksArray.size());
    for (const QJsonValue& askValue : asksArray) {
        snapshot.asks.push_back(parseOrderLevel(askValue.toObject()));
    }
    
    return snapshot;
}

OrderBookLevel TestDataPlayer::parseOrderLevel(const QJsonObject& json) {
    OrderBookLevel level;
    level.price = json["price"].toDouble();
    level.size = json["size"].toDouble();
    return level;
}

void TestDataPlayer::startPlayback(int interval_ms) {
    if (m_snapshots.empty()) {
        sLog_Init("❌ No test data loaded - cannot start playback");
        return;
    }
    
    m_baseInterval_ms = interval_ms;
    updatePlaybackTimer();
    
    sLog_Init("🎬 Starting test data playback:");
    sLog_Init("   Snapshots:" << m_snapshots.size());
    sLog_Init("   Base interval:" << m_baseInterval_ms << "ms");
    sLog_Init("   Speed multiplier:" << m_speedMultiplier << "x");
    sLog_Init("   Effective interval:" << (m_baseInterval_ms / m_speedMultiplier) << "ms");
    
    m_playbackTimer.start();
}

void TestDataPlayer::stopPlayback() {
    m_playbackTimer.stop();
    sLog_Init("⏹️ Test data playback stopped");
}

void TestDataPlayer::setPlaybackSpeed(double multiplier) {
    m_speedMultiplier = multiplier;
    updatePlaybackTimer();
    
    sLog_Init("🚀 Playback speed changed to" << multiplier << "x");
}

void TestDataPlayer::updatePlaybackTimer() {
    if (m_playbackTimer.isActive()) {
        int effectiveInterval = static_cast<int>(m_baseInterval_ms / m_speedMultiplier);
        effectiveInterval = std::max(1, effectiveInterval);  // Minimum 1ms
        m_playbackTimer.setInterval(effectiveInterval);
    }
}

double TestDataPlayer::getProgress() const {
    if (m_snapshots.empty()) return 0.0;
    return static_cast<double>(m_currentIndex) / static_cast<double>(m_snapshots.size());
}

void TestDataPlayer::seekToTime(int64_t timestamp_ms) {
    // Find the closest snapshot to the target timestamp
    size_t bestIndex = 0;
    int64_t bestDelta = std::abs(m_snapshots[0].timestamp - timestamp_ms);
    
    for (size_t i = 1; i < m_snapshots.size(); ++i) {
        int64_t delta = std::abs(m_snapshots[i].timestamp - timestamp_ms);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = i;
        }
    }
    
    seekToIndex(bestIndex);
}

void TestDataPlayer::seekToIndex(size_t index) {
    if (index >= m_snapshots.size()) {
        index = m_snapshots.size() - 1;
    }
    
    m_currentIndex = index;
    emit progressChanged(getProgress());
    
    sLog_Init("⏯️ Seeked to index" << index << "(" << (getProgress() * 100.0) << "%)");
}

QString TestDataPlayer::getMetadata() const {
    QString info;
    info += QString("Generated: %1\n").arg(m_metadata["generated_at"].toString());
    info += QString("Duration: %1 hours\n").arg(m_metadata["duration_hours"].toDouble());
    info += QString("Interval: %1ms\n").arg(m_metadata["interval_ms"].toInt());
    info += QString("Snapshots: %1\n").arg(m_metadata["num_snapshots"].toInt());
    
    QJsonObject coverage = m_metadata["timeframe_coverage"].toObject();
    info += "Timeframe Coverage:\n";
    for (auto it = coverage.begin(); it != coverage.end(); ++it) {
        info += QString("  %1: %2 points\n").arg(it.key()).arg(it.value().toInt());
    }
    
    return info;
}

void TestDataPlayer::onPlaybackTick() {
    if (m_currentIndex >= m_snapshots.size()) {
        stopPlayback();
        emit playbackFinished();
        sLog_Init("🎬 Test data playback finished");
        return;
    }
    
    const OrderBookSnapshot& snapshot = m_snapshots[m_currentIndex];
    
    // Convert to OrderBook structure
    OrderBook book;
    book.bids = snapshot.bids;
    book.asks = snapshot.asks;
    // Note: OrderBook struct may not have timestamp field
    
    // Emit the order book update
    emit orderBookUpdated(book);
    
    // Progress tracking
    static size_t lastProgressReport = 0;
    if (m_currentIndex - lastProgressReport >= 1000) {  // Report every 1000 updates
        double progress = getProgress() * 100.0;
        sLog_Init("📊 Test playback progress:" << progress << "% (" << m_currentIndex << "/" << m_snapshots.size() << ")");
        emit progressChanged(getProgress());
        lastProgressReport = m_currentIndex;
    }
    
    m_currentIndex++;
} 