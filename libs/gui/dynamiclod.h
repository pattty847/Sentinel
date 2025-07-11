// libs/gui/dynamiclod.h
#pragma once

#include <QObject>
#include <vector>
#include <cstdint>

class DynamicLOD : public QObject {
    Q_OBJECT
    
public:
    // Exponential timeframe progression for smooth LOD transitions
    enum class TimeFrame : int {
        TF_100ms = 0,   // Ultra-high frequency
        TF_250ms = 1,   // High frequency  
        TF_500ms = 2,   // Medium-high frequency
        TF_1s = 3,      // Second-level
        TF_2s = 4,      // 2-second buckets
        TF_5s = 5,      // 5-second buckets
        TF_15s = 6,     // 15-second buckets
        TF_30s = 7,     // 30-second buckets
        TF_1m = 8,      // Minute-level
        TF_2m = 9,      // 2-minute buckets
        TF_5m = 10,     // 5-minute buckets
        TF_15m = 11,    // 15-minute buckets
        TF_30m = 12,    // 30-minute buckets
        TF_1h = 13,     // Hour-level
        TF_4h = 14,     // 4-hour buckets
        TF_12h = 15,    // 12-hour buckets
        TF_1d = 16,     // Daily
        COUNT = 17
    };
    
    explicit DynamicLOD(QObject* parent = nullptr);
    
    // Perfect bucket alignment - no overlaps!
    static int64_t alignTimestamp(int64_t timestamp_ms, TimeFrame tf) {
        int64_t duration = getTimeFrameDuration(tf);
        return (timestamp_ms / duration) * duration;
    }
    
    // Deterministic timeframe selection based on zoom level
    TimeFrame selectOptimalTimeFrame(double msPerPixel, double targetPixelsPerBucket = 20.0) const;
    
    // Grid calculation for perfect bucket spacing
    struct GridInfo {
        TimeFrame timeFrame;
        int64_t bucketDuration_ms;
        int64_t firstBucketStart_ms;
        int64_t lastBucketStart_ms;
        int totalBuckets;
        double pixelsPerBucket;
    };
    
    GridInfo calculateGrid(int64_t viewStart_ms, int64_t viewEnd_ms, double pixelWidth) const;
    
    static int64_t getTimeFrameDuration(TimeFrame tf) {
        static constexpr int64_t durations[] = {
            100,     // 100ms
            250,     // 250ms
            500,     // 500ms
            1000,    // 1s
            2000,    // 2s
            5000,    // 5s
            15000,   // 15s
            30000,   // 30s
            60000,   // 1m
            120000,  // 2m
            300000,  // 5m
            900000,  // 15m
            1800000, // 30m
            3600000, // 1h
            14400000,// 4h
            43200000,// 12h
            86400000 // 1d
        };
        return durations[static_cast<int>(tf)];
    }
    
    static const char* getTimeFrameName(TimeFrame tf) {
        static constexpr const char* names[] = {
            "100ms", "250ms", "500ms", "1s", "2s", "5s", "15s", "30s",
            "1m", "2m", "5m", "15m", "30m", "1h", "4h", "12h", "1d"
        };
        return names[static_cast<int>(tf)];
    }
    
signals:
    void timeFrameChanged(TimeFrame newTF, const GridInfo& gridInfo);
    
private:
    TimeFrame m_currentTimeFrame = TimeFrame::TF_1s;
};