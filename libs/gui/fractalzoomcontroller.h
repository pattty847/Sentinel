#pragma once
#include <QObject>
#include <QTimer>
#include "candlelod.h"

enum class ZoomMode {
    Both,      // Default - zoom both X and Y axes
    TimeOnly,  // X-axis only (time zoom)
    PriceOnly, // Y-axis only (price zoom)
    Lock       // Maintain current zoom, pan only
};

class FractalZoomController : public QObject {
    Q_OBJECT

public:
    explicit FractalZoomController(QObject* parent = nullptr);
    
    // 🔥 CORE FRACTAL ZOOM LOGIC
    CandleLOD::TimeFrame selectOptimalTimeFrameForDensity(
        double pixelsPerCandle, 
        size_t targetCandleCount = 300) const;
    
    // 🎯 COORDINATED TEMPORAL RESOLUTION
    int getOptimalOrderBookUpdateInterval(CandleLOD::TimeFrame timeframe) const;
    int getOptimalTradePointUpdateInterval(CandleLOD::TimeFrame timeframe) const;
    size_t getOptimalOrderBookDepth(CandleLOD::TimeFrame timeframe) const;
    
    // 🚀 ZOOM MODE MANAGEMENT
    void setZoomMode(ZoomMode mode) { m_zoomMode = mode; }
    ZoomMode getZoomMode() const { return m_zoomMode; }
    
    // 🔍 INTELLIGENT AGGREGATION TRIGGER
    bool shouldTriggerAggregation(CandleLOD::TimeFrame currentTF, double zoomLevel) const;
    
    // 🕯️ ADAPTIVE "NOW CANDLE" MANAGEMENT
    bool hasCurrentBuildingCandle(CandleLOD::TimeFrame tf) const;
    
    // 📊 ZOOM LEVEL ANALYTICS
    double calculateZoomLevel(int64_t timeSpan_ms, double viewWidth) const;
    QString getZoomLevelDescription(CandleLOD::TimeFrame tf) const;

public slots:
    // 🎯 COORDINATED UPDATES - All layers synchronized
    void onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame);
    void onZoomLevelChanged(double zoomLevel);
    
signals:
    // 🔥 TEMPORAL COORDINATION SIGNALS
    void orderBookUpdateIntervalChanged(int intervalMs);
    void tradePointUpdateIntervalChanged(int intervalMs);
    void orderBookDepthChanged(size_t maxLevels);
    void timeFrameChanged(CandleLOD::TimeFrame newTimeFrame);
    void zoomModeChanged(ZoomMode mode);

private:
    ZoomMode m_zoomMode = ZoomMode::Both;
    CandleLOD::TimeFrame m_currentTimeFrame = CandleLOD::TF_1min;
    double m_currentZoomLevel = 1.0;
    
    // 🎯 TEMPORAL RESOLUTION MAPPING
    struct TemporalSettings {
        int orderBookInterval_ms;
        int tradePointInterval_ms;
        size_t orderBookDepth;
        const char* description;
    };
    
    static const TemporalSettings TEMPORAL_SETTINGS[];
    
    // 🔍 ZOOM LEVEL CALCULATION
    double calculatePixelsPerCandle(double viewWidth, int64_t timeSpan_ms, 
                                   CandleLOD::TimeFrame tf) const;
}; 