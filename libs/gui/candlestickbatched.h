#pragma once

#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGTransformNode>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include "candlelod.h"
#include "tradedata.h"
struct CandleUpdate;

// 🕯️ BATCHED CANDLESTICK RENDERING: Professional trading chart candles
// Two-draw-call architecture: green candles vs red candles for optimal GPU performance

class CandlestickBatched : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    
    // 🎯 QML PROPERTIES: Configurable candle appearance
    Q_PROPERTY(bool lodEnabled READ lodEnabled WRITE setLodEnabled NOTIFY lodEnabledChanged)
    Q_PROPERTY(double candleWidth READ candleWidth WRITE setCandleWidth NOTIFY candleWidthChanged)
    Q_PROPERTY(bool volumeScaling READ volumeScaling WRITE setVolumeScaling NOTIFY volumeScalingChanged)
    Q_PROPERTY(int maxCandles READ maxCandles WRITE setMaxCandles NOTIFY maxCandlesChanged)
    
public:
    explicit CandlestickBatched(QQuickItem* parent = nullptr);
    
    Q_INVOKABLE void clearCandles();
    Q_INVOKABLE void setTimeWindow(int64_t startTime_ms, int64_t endTime_ms);
    
    // 🎯 LOD CONTROL: Performance optimization
    Q_INVOKABLE void setAutoLOD(bool enabled);
    Q_INVOKABLE void forceTimeFrame(int timeframe); // CandleLOD::TimeFrame values
    Q_INVOKABLE double calculateCurrentPixelsPerCandle() const;
    
    // 🎨 APPEARANCE CONTROL
    Q_INVOKABLE void setBullishColor(const QColor& color);
    Q_INVOKABLE void setBearishColor(const QColor& color);
    Q_INVOKABLE void setWickColor(const QColor& color);
    
    // Property getters
    bool lodEnabled() const { return m_lodEnabled; }
    double candleWidth() const { return m_candleWidth; }
    bool volumeScaling() const { return m_volumeScaling; }
    int maxCandles() const { return m_maxCandles; }

public slots:
    // 🔥 INTEGRATION: Connect to chart coordinate system
    void onViewChanged(int64_t startTimeMs, int64_t endTimeMs, double minPrice, double maxPrice);
    void onCandlesReady(const std::vector<CandleUpdate>& candles);

signals:
    // Property change notifications
    void lodEnabledChanged();
    void candleWidthChanged();
    void volumeScalingChanged();
    void maxCandlesChanged();
    
    // 🎯 PERFORMANCE SIGNALS: For monitoring
    void candleCountChanged(int count);
    void lodLevelChanged(int timeframe);
    void renderTimeChanged(double ms);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    // 🕯️ CANDLE GEOMETRY: GPU vertex structure for efficient rendering
    struct CandleVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
    };
    
    struct CandleInstance {
        OHLC candle;
        QColor color;
        float screenX;
        float bodyTop, bodyBottom;   // Screen Y coordinates
        float wickTop, wickBottom;
        float width;
    };
    
    // 🔥 TWO-DRAW-CALL SEPARATION: Optimize GPU by color
    struct RenderBatch {
        std::vector<CandleInstance> bullishCandles;  // Green candles
        std::vector<CandleInstance> bearishCandles;  // Red candles
        bool geometryDirty = true;
    };
    
    // 🎯 CORE DATA: Render state and candle storage
    std::array<std::vector<OHLC>, CandleLOD::NUM_TIMEFRAMES> m_candles;
    RenderBatch m_renderBatch;
    std::mutex m_dataMutex;
    std::atomic<bool> m_geometryDirty{true};
    
    // 🔥 COORDINATE SYSTEM: Match with GPUChartWidget
    int64_t m_viewStartTime_ms = 0;
    int64_t m_viewEndTime_ms = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    bool m_coordinatesValid = false;
    
    // 🎨 APPEARANCE SETTINGS
    QColor m_bullishColor = QColor(0, 255, 0, 180);    // Green with transparency
    QColor m_bearishColor = QColor(255, 0, 0, 180);    // Red with transparency  
    QColor m_wickColor = QColor(255, 255, 255, 200);   // White wicks
    
    // 🎯 CONFIGURATION
    bool m_lodEnabled = true;
    double m_candleWidth = 8.0;      // Base candle width in pixels
    bool m_volumeScaling = true;     // Scale width by volume
    int m_maxCandles = 10000;        // Maximum candles to render
    bool m_autoLOD = true;           // Automatic LOD selection
    CandleLOD::TimeFrame m_forcedTimeFrame = CandleLOD::TF_1min;
    
    // 🔥 PERFORMANCE TRACKING
    mutable std::chrono::high_resolution_clock::time_point m_lastRenderTime;
    mutable double m_lastRenderDuration_ms = 0.0;
    
    
    // Helper methods
    void updateRenderBatch();
    void separateAndUpdateCandles(const std::vector<OHLC>& candles);
    QPointF worldToScreen(int64_t timestamp_ms, double price) const;
    void createCandleGeometry(QSGGeometryNode* node, const std::vector<CandleInstance>& candles, 
                             const QColor& color, bool isBody);
    
    // 🔥 GPU GEOMETRY CREATION: Efficient vertex buffer construction
    void buildCandleBodies(const std::vector<CandleInstance>& candles, 
                          std::vector<CandleVertex>& vertices) const;
    void buildCandleWicks(const std::vector<CandleInstance>& candles, 
                         std::vector<CandleVertex>& vertices) const;
    
    // 🎯 LOD MANAGEMENT
    CandleLOD::TimeFrame selectOptimalTimeFrame() const;
    void updateLODIfNeeded();
    
    // Property setters
    void setLodEnabled(bool enabled);
    void setCandleWidth(double width);
    void setVolumeScaling(bool enabled);
    void setMaxCandles(int max);
}; 