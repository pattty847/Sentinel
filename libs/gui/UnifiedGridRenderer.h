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
 *  UNIFIED GRID RENDERER - SLIM QML ADAPTER
 * 
 * Slim QML adapter that delegates to the V2 modular architecture.
 * Maintains QML interface compatibility while using the new modular system.
 */
class UnifiedGridRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(RenderMode renderMode READ renderMode WRITE setRenderMode NOTIFY renderModeChanged)
    Q_PROPERTY(bool showVolumeProfile READ showVolumeProfile WRITE setShowVolumeProfile NOTIFY showVolumeProfileChanged)
    Q_PROPERTY(double intensityScale READ intensityScale WRITE setIntensityScale NOTIFY intensityScaleChanged)
    Q_PROPERTY(int maxCells READ maxCells WRITE setMaxCells NOTIFY maxCellsChanged)
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
    Q_PROPERTY(double minVolumeFilter READ minVolumeFilter WRITE setMinVolumeFilter NOTIFY minVolumeFilterChanged)
    Q_PROPERTY(double currentPriceResolution READ getCurrentPriceResolution NOTIFY priceResolutionChanged)
    
    // Trade Bubble Properties
    Q_PROPERTY(double minBubbleRadius READ minBubbleRadius WRITE setMinBubbleRadius NOTIFY minBubbleRadiusChanged)
    Q_PROPERTY(double maxBubbleRadius READ maxBubbleRadius WRITE setMaxBubbleRadius NOTIFY maxBubbleRadiusChanged)
    Q_PROPERTY(double bubbleOpacity READ bubbleOpacity WRITE setBubbleOpacity NOTIFY bubbleOpacityChanged)
    
    // Overlay Properties for Layered Rendering
    Q_PROPERTY(bool showHeatmapLayer READ showHeatmapLayer WRITE setShowHeatmapLayer NOTIFY showHeatmapLayerChanged)
    Q_PROPERTY(bool showTradeBubbleLayer READ showTradeBubbleLayer WRITE setShowTradeBubbleLayer NOTIFY showTradeBubbleLayerChanged)
    Q_PROPERTY(bool showTradeFlowLayer READ showTradeFlowLayer WRITE setShowTradeFlowLayer NOTIFY showTradeFlowLayerChanged)
    
    Q_PROPERTY(qint64 visibleTimeStart READ getVisibleTimeStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 visibleTimeEnd READ getVisibleTimeEnd NOTIFY viewportChanged)
    Q_PROPERTY(double minPrice READ getMinPrice NOTIFY viewportChanged)
    Q_PROPERTY(double maxPrice READ getMaxPrice NOTIFY viewportChanged)
    
    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)
    
    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)

public:
    enum class RenderMode {
        LiquidityHeatmap,    // Bookmap-style dense grid
        TradeFlow,           // Trade dots with density
        TradeBubbles,        // Size-relative bubbles on heatmap
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
    
    // Trade Bubble configuration
    double m_minBubbleRadius = 4.0;      // Minimum bubble size (pixels)
    double m_maxBubbleRadius = 20.0;     // Maximum bubble size (pixels) 
    double m_bubbleOpacity = 0.85;       // Base opacity for bubbles
    
    // Overlay layer toggles
    bool m_showHeatmapLayer = true;      // Base heatmap layer
    bool m_showTradeBubbleLayer = true;  // Trade bubble overlay
    bool m_showTradeFlowLayer = false;   // Trade flow overlay
    
    bool m_manualTimeframeSet = false;  // Disable auto-suggestion when user manually sets timeframe
    QElapsedTimer m_manualTimeframeTimer;  // Reset auto-suggestion after delay
    
    // Thread safety
    mutable std::mutex m_dataMutex;
    
    //  FOUR DIRTY FLAGS SYSTEM
    // Each flag triggers a different update path in updatePaintNode
    std::atomic<bool> m_geometryDirty{true};     // Topology/LOD/mode changed (RARE - full rebuild)
    std::atomic<bool> m_appendPending{false};    // New data arrived (COMMON - append cells)
    std::atomic<bool> m_transformDirty{false};   // Pan/zoom/follow (VERY COMMON - transform only)
    std::atomic<bool> m_materialDirty{false};    // Visual params changed (OCCASIONAL - uniforms/material)
        
    // Rendering data
    std::vector<CellInstance> m_visibleCells;
    // Snapshot buffer swapped from DataProcessor on dataUpdated()/updatePaintNode
    std::shared_ptr<const std::vector<CellInstance>> m_publishedCells;
    std::vector<Trade> m_recentTrades;  // Recent trades for bubble rendering
    std::vector<std::pair<double, double>> m_volumeProfile;
    
    QSGTransformNode* m_rootTransformNode = nullptr;
    bool m_needsDataRefresh = false;
    bool m_panSyncPending = false;  // hold visual pan until DP resync snapshot applied to avoid snap-back -- TODO: See if this is needed
    
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
    
    // Trade Bubble accessors
    double minBubbleRadius() const { return m_minBubbleRadius; }
    double maxBubbleRadius() const { return m_maxBubbleRadius; }
    double bubbleOpacity() const { return m_bubbleOpacity; }
    
    // Overlay layer accessors
    bool showHeatmapLayer() const { return m_showHeatmapLayer; }
    bool showTradeBubbleLayer() const { return m_showTradeBubbleLayer; }
    bool showTradeFlowLayer() const { return m_showTradeFlowLayer; }
    
    //  VIEWPORT BOUNDS: Getters for QML properties
    qint64 getVisibleTimeStart() const;
    qint64 getVisibleTimeEnd() const; 
    double getMinPrice() const;
    double getMaxPrice() const;
    
    //  OPTIMIZATION 4: QML-compatible timeframe getter (returns int for Q_PROPERTY)
    int getCurrentTimeframe() const { return static_cast<int>(m_currentTimeframe_ms); }
    
    //  VISUAL TRANSFORM: Getter for QML pan offset
    QPointF getPanVisualOffset() const;
    
    //  DATA INTERFACE
    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    
    //  GRID CONFIGURATION
    Q_INVOKABLE void setPriceResolution(double resolution);
    Q_INVOKABLE int getCurrentTimeResolution() const;
    Q_INVOKABLE double getCurrentPriceResolution() const;
    Q_INVOKABLE void setGridResolution(int timeResMs, double priceRes);
    struct GridResolution {
        int timeMs;
        double price;
    };
    static GridResolution calculateOptimalResolution(qint64 timeSpanMs, double priceSpan, int targetVerticalLines = 10, int targetHorizontalLines = 15);
    
    //  DEBUG: Check grid system state
    Q_INVOKABLE QString getGridDebugInfo() const;
    
    //  DEBUG: Detailed grid debug information
    Q_INVOKABLE QString getDetailedGridDebug() const;
    
    //  PERFORMANCE MONITORING API
    Q_INVOKABLE void togglePerformanceOverlay();
    Q_INVOKABLE QString getPerformanceStats() const;
    Q_INVOKABLE double getCurrentFPS() const;
    Q_INVOKABLE double getAverageRenderTime() const;
    Q_INVOKABLE double getCacheHitRate() const;
    
    //  GRID SYSTEM CONTROLS
    Q_INVOKABLE void setGridMode(int mode);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    void setDataCache(class DataCache* cache); // Forward declaration - implemented in .cpp
    void setSentinelMonitor(std::shared_ptr<SentinelMonitor> monitor) { m_sentinelMonitor = monitor; }
    
    //  PAN/ZOOM CONTROLS
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
    
    //  COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
    Q_INVOKABLE QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    Q_INVOKABLE QPointF screenToWorld(double screenX, double screenY) const;
    Q_INVOKABLE double getScreenWidth() const;
    Q_INVOKABLE double getScreenHeight() const;

public:
    // Real-time data integration
    void onTradeReceived(const Trade& trade);
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
    void minBubbleRadiusChanged();
    void maxBubbleRadiusChanged(); 
    void bubbleOpacityChanged();
    void showHeatmapLayerChanged();
    void showTradeBubbleLayerChanged();
    void showTradeFlowLayerChanged();
    void viewportChanged();
    void timeframeChanged();
    void panVisualOffsetChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    
    //  MOUSE INTERACTION EVENTS
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
    void setMinBubbleRadius(double radius);
    void setMaxBubbleRadius(double radius);
    void setBubbleOpacity(double opacity);
    void setShowHeatmapLayer(bool show);
    void setShowTradeBubbleLayer(bool show);
    void setShowTradeFlowLayer(bool show);
    void updateVisibleCells();
    void updateVolumeProfile();
    
    class DataCache* m_dataCache = nullptr;
    
    std::unique_ptr<GridViewState> m_viewState;
    std::shared_ptr<SentinelMonitor> m_sentinelMonitor;
    std::unique_ptr<DataProcessor> m_dataProcessor;
    std::unique_ptr<QThread> m_dataProcessorThread;
    std::unique_ptr<IRenderStrategy> m_heatmapStrategy;
    std::unique_ptr<IRenderStrategy> m_tradeFlowStrategy;  
    std::unique_ptr<IRenderStrategy> m_tradeBubbleStrategy;
    std::unique_ptr<IRenderStrategy> m_candleStrategy;

    IRenderStrategy* getCurrentStrategy() const;
    
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_dataProcessor.get(); }
};