// ======= libs/gui/UnifiedGridRenderer.h =======
/*
Sentinel — UnifiedGridRenderer
Role: A QML scene item that orchestrates multiple rendering strategies to draw market data.
Inputs/Outputs: Takes Trade/OrderBook data via slots; outputs a QSGNode tree for GPU rendering.
Threading: Runs on the GUI thread; data is received via queued connections; rendering on render thread.
Performance: Batches incoming data with a QTimer to throttle scene graph updates.
Integration: Used in QML; receives data from MarketDataCore; delegates rendering to strategy objects.
Observability: Logs key events like data reception and paint node updates via qDebug.
Related: UnifiedGridRenderer.cpp, IRenderStrategy.h, CoordinateSystem.h, MarketDataCore.hpp.
Assumptions: CoordinateSystem and ChartModeController properties are set from QML.
*/
#pragma once
#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include "../core/marketdata/model/TradeData.h"
#include "render/GridTypes.hpp"
#include "render/GridViewState.hpp"

// Forward declarations for new modular architecture
class GridSceneNode;
class SentinelMonitor;
class DataProcessor;
class IRenderStrategy;

/**
 * 🎯 UNIFIED GRID RENDERER - SLIM QML ADAPTER
 * 
 * Slim QML adapter that delegates to the V2 modular architecture.
 * Maintains QML interface compatibility while using the new modular system.
 */
class UnifiedGridRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    
    // Rendering mode selection
    Q_PROPERTY(RenderMode renderMode READ renderMode WRITE setRenderMode NOTIFY renderModeChanged)
    Q_PROPERTY(bool showVolumeProfile READ showVolumeProfile WRITE setShowVolumeProfile NOTIFY showVolumeProfileChanged)
    Q_PROPERTY(double intensityScale READ intensityScale WRITE setIntensityScale NOTIFY intensityScaleChanged)
    Q_PROPERTY(int maxCells READ maxCells WRITE setMaxCells NOTIFY maxCellsChanged)
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
    Q_PROPERTY(double minVolumeFilter READ minVolumeFilter WRITE setMinVolumeFilter NOTIFY minVolumeFilterChanged)
    Q_PROPERTY(double currentPriceResolution READ getCurrentPriceResolution NOTIFY priceResolutionChanged)
    
    // 🚀 VIEWPORT BOUNDS: Expose current viewport to QML for dynamic axis labels
    Q_PROPERTY(qint64 visibleTimeStart READ getVisibleTimeStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 visibleTimeEnd READ getVisibleTimeEnd NOTIFY viewportChanged)
    Q_PROPERTY(double minPrice READ getMinPrice NOTIFY viewportChanged)
    Q_PROPERTY(double maxPrice READ getMaxPrice NOTIFY viewportChanged)
    
    // 🚀 OPTIMIZATION 4: Timeframe property with proper QML binding
    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)
    
    // 🚀 VISUAL TRANSFORM: Expose pan offset for real-time grid sync
    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)

public:
    enum class RenderMode {
        LiquidityHeatmap,    // Bookmap-style dense grid
        TradeFlow,           // Trade dots with density
        VolumeCandles,       // Volume-weighted candles
        OrderBookDepth       // Depth chart style
    };
    Q_ENUM(RenderMode)

private:
    // Rendering configuration
    RenderMode m_renderMode = RenderMode::LiquidityHeatmap;
    bool m_showVolumeProfile = true;
    double m_intensityScale = 1.0;
    int m_maxCells = 100000;
    double m_minVolumeFilter = 0.0;      // Volume filter
    int64_t m_currentTimeframe_ms = 100;  // Default to 100ms for smooth updates
    
    // 🐛 FIX: Manual timeframe override tracking
    bool m_manualTimeframeSet = false;  // Disable auto-suggestion when user manually sets timeframe
    QElapsedTimer m_manualTimeframeTimer;  // Reset auto-suggestion after delay
    
    // Thread safety
    mutable std::mutex m_dataMutex;
    std::atomic<bool> m_geometryDirty{true};
    
    // CellInstance now defined in render/GridTypes.hpp
    
    // Rendering data
    std::vector<CellInstance> m_visibleCells;
    std::vector<std::pair<double, double>> m_volumeProfile;
    
    // Legacy geometry cache (simplified for V2)
    struct CachedGeometry {
        QSGGeometryNode* node = nullptr;
        QMatrix4x4 originalTransform;
        std::vector<CellInstance> cachedCells;
        bool isValid = false;
        int64_t cacheTimeStart_ms = 0;
        int64_t cacheTimeEnd_ms = 0;
        double cacheMinPrice = 0.0;
        double cacheMaxPrice = 0.0;
    };
    
    CachedGeometry m_geometryCache;
    QSGTransformNode* m_rootTransformNode = nullptr;
    bool m_needsDataRefresh = false;
    
    // V1 state (removed - now delegated to DataProcessor)

public:
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer(); // Custom destructor needed for unique_ptr with incomplete types
    
    // Property accessors
    RenderMode renderMode() const { return m_renderMode; }
    bool showVolumeProfile() const { return m_showVolumeProfile; }
    double intensityScale() const { return m_intensityScale; }
    int maxCells() const { return m_maxCells; }
    int64_t currentTimeframe() const { return m_currentTimeframe_ms; }
    double minVolumeFilter() const { return m_minVolumeFilter; }
    bool autoScrollEnabled() const { return m_viewState ? m_viewState->isAutoScrollEnabled() : false; }
    
    // 🚀 VIEWPORT BOUNDS: Getters for QML properties
    qint64 getVisibleTimeStart() const;
    qint64 getVisibleTimeEnd() const; 
    double getMinPrice() const;
    double getMaxPrice() const;
    
    // 🚀 OPTIMIZATION 4: QML-compatible timeframe getter (returns int for Q_PROPERTY)
    int getCurrentTimeframe() const { return static_cast<int>(m_currentTimeframe_ms); }
    
    // 🚀 VISUAL TRANSFORM: Getter for QML pan offset
    QPointF getPanVisualOffset() const;
    
    // 🎯 DATA INTERFACE
    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    
    // 🎯 GRID CONFIGURATION
    Q_INVOKABLE void setPriceResolution(double resolution);
    Q_INVOKABLE int getCurrentTimeResolution() const;
    Q_INVOKABLE double getCurrentPriceResolution() const;
    Q_INVOKABLE void setGridResolution(int timeResMs, double priceRes);
    struct GridResolution {
        int timeMs;
        double price;
    };
    static GridResolution calculateOptimalResolution(qint64 timeSpanMs, double priceSpan, int targetVerticalLines = 10, int targetHorizontalLines = 15);
    
    // 🔥 DEBUG: Check grid system state
    Q_INVOKABLE QString getGridDebugInfo() const;
    
    // 🔥 DEBUG: Detailed grid debug information
    Q_INVOKABLE QString getDetailedGridDebug() const;
    
    // 📊 PERFORMANCE MONITORING API
    Q_INVOKABLE void togglePerformanceOverlay();
    Q_INVOKABLE QString getPerformanceStats() const;
    Q_INVOKABLE double getCurrentFPS() const;
    Q_INVOKABLE double getAverageRenderTime() const;
    Q_INVOKABLE double getCacheHitRate() const;
    
    // 🔥 GRID SYSTEM CONTROLS
    Q_INVOKABLE void setGridMode(int mode);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    void setDataCache(class DataCache* cache); // Forward declaration - implemented in .cpp
    void setSentinelMonitor(std::shared_ptr<SentinelMonitor> monitor) { m_sentinelMonitor = monitor; }
    
    // 🔥 PAN/ZOOM CONTROLS
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
    
    // 🎯 COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
    Q_INVOKABLE QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    Q_INVOKABLE QPointF screenToWorld(double screenX, double screenY) const;
    Q_INVOKABLE double getScreenWidth() const;
    Q_INVOKABLE double getScreenHeight() const;

public slots:
    // Real-time data integration
    void onTradeReceived(const Trade& trade);
    
    // Dense-only order book signal (no sparse conversion)
    void onLiveOrderBookUpdated(const QString& productId);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    
    // Automatic price resolution adjustment on viewport changes
    void onViewportChanged();

signals:
    void renderModeChanged();
    void showVolumeProfileChanged();
    void intensityScaleChanged();
    void maxCellsChanged();
    void gridResolutionChanged(int timeRes_ms, double priceRes);
    void autoScrollEnabledChanged();
    void minVolumeFilterChanged();
    void priceResolutionChanged();
    void viewportChanged();
    void timeframeChanged();
    void panVisualOffsetChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);
    
    // 🖱️ MOUSE INTERACTION EVENTS
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // Property setters
    void setRenderMode(RenderMode mode);
    void setShowVolumeProfile(bool show);
    void setIntensityScale(double scale);
    void setMaxCells(int max);
    void setMinVolumeFilter(double minVolume);
    void updateVisibleCells();
    void updateVolumeProfile();
    
    class DataCache* m_dataCache = nullptr;
    
    std::unique_ptr<GridViewState> m_viewState;
    std::shared_ptr<SentinelMonitor> m_sentinelMonitor;
    std::unique_ptr<DataProcessor> m_dataProcessor;
    std::unique_ptr<QThread> m_dataProcessorThread;
    std::unique_ptr<IRenderStrategy> m_heatmapStrategy;
    std::unique_ptr<IRenderStrategy> m_tradeFlowStrategy;  
    std::unique_ptr<IRenderStrategy> m_candleStrategy;

    IRenderStrategy* getCurrentStrategy() const;
    
    void initializeV2Architecture();
    
    QSGNode* updatePaintNodeV2(QSGNode* oldNode);

public:
    DataProcessor* getDataProcessor() const { return m_dataProcessor.get(); }
};