// ======= libs/gui/UnifiedGridRenderer.h =======
#pragma once
#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include "LiquidityTimeSeriesEngine.h"
#include "TradeData.h"

/**
 * 🎯 UNIFIED GRID RENDERER
 * 
 * Single rendering component that can display:
 * - Dense liquidity heatmap (Bookmap style)
 * - Trade flow visualization
 * - Volume profile bars
 * - Aggregated OHLC candles
 * 
 * All using the same unified coordinate system with proper LOD.
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
    // 🚀 OPTIMIZATION 4: Timeframe property with proper QML binding
    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)

public:
    enum class RenderMode {
        LiquidityHeatmap,    // Bookmap-style dense grid
        TradeFlow,           // Trade dots with density
        VolumeCandles,       // Volume-weighted candles
        OrderBookDepth       // Depth chart style
    };
    Q_ENUM(RenderMode)

private:
    // Core liquidity engine
    std::unique_ptr<LiquidityTimeSeriesEngine> m_liquidityEngine;
    QTimer* m_orderBookTimer;
    
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
    
    // GPU vertex structures
    struct GridVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float intensity;      // Volume/liquidity intensity
    };
    
    struct CellInstance {
        QRectF screenRect;
        QColor color;
        double intensity;
        double liquidity;
        bool isBid;
        int64_t timeSlot;
        double priceLevel;
    };
    
    // Rendering data
    std::vector<CellInstance> m_visibleCells;
    std::vector<std::pair<double, double>> m_volumeProfile;
    
    // 🚀 PERSISTENT GEOMETRY CACHE (for performance)
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
    
    // 📊 PROFESSIONAL PERFORMANCE MONITORING (like TradingView/Bookmap)
    struct PerformanceMetrics {
        QElapsedTimer frameTimer;
        std::vector<qint64> frameTimes;  // Last 60 frame times in microseconds
        qint64 lastFrameTime_us = 0;
        qint64 renderTime_us = 0;
        qint64 cacheHits = 0;
        qint64 cacheMisses = 0;
        qint64 geometryRebuilds = 0;
        qint64 transformsApplied = 0;
        size_t cachedCellCount = 0;
        bool showOverlay = false;
        
        double getCurrentFPS() const {
            if (frameTimes.size() < 2) return 0.0;
            qint64 totalTime = 0;
            for (qint64 time : frameTimes) totalTime += time;
            return (frameTimes.size() - 1) * 1000000.0 / totalTime;  // Convert μs to FPS
        }
        
        double getAverageRenderTime_ms() const {
            if (frameTimes.empty()) return 0.0;
            qint64 totalTime = 0;
            for (qint64 time : frameTimes) totalTime += time;
            return totalTime / (frameTimes.size() * 1000.0);  // Convert μs to ms
        }
        
        double getCacheHitRate() const {
            qint64 total = cacheHits + cacheMisses;
            return (total > 0) ? (cacheHits * 100.0 / total) : 0.0;
        }
    };
    
    mutable PerformanceMetrics m_perfMetrics;
    mutable size_t m_bytesUploadedThisFrame = 0;
    mutable std::atomic<size_t> m_totalBytesUploaded{0};
    static constexpr double PCIE_BUDGET_MB_PER_SECOND = 200.0;

public:
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    
    // Property accessors
    RenderMode renderMode() const { return m_renderMode; }
    bool showVolumeProfile() const { return m_showVolumeProfile; }
    double intensityScale() const { return m_intensityScale; }
    int maxCells() const { return m_maxCells; }
    int64_t currentTimeframe() const { return m_currentTimeframe_ms; }
    double minVolumeFilter() const { return m_minVolumeFilter; }
    bool autoScrollEnabled() const { return m_autoScrollEnabled; }
    
    // 🚀 OPTIMIZATION 4: QML-compatible timeframe getter (returns int for Q_PROPERTY)
    int getCurrentTimeframe() const { return static_cast<int>(m_currentTimeframe_ms); }
    
    // 🎯 DATA INTERFACE
    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void updateOrderBook(const OrderBook& orderBook);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    
    // 🎯 GRID CONFIGURATION
    Q_INVOKABLE void setTimeResolution(int resolution_ms);
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
    Q_INVOKABLE double getCurrentFPS() const { return m_perfMetrics.getCurrentFPS(); }
    Q_INVOKABLE double getAverageRenderTime() const { return m_perfMetrics.getAverageRenderTime_ms(); }
    Q_INVOKABLE double getCacheHitRate() const { return m_perfMetrics.getCacheHitRate(); }
    
    // 🔥 GRID SYSTEM CONTROLS
    Q_INVOKABLE void setGridMode(int mode);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    // 🔥 PAN/ZOOM CONTROLS
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);

public slots:
    // Real-time data integration
    void onTradeReceived(const Trade& trade);
    void onOrderBookUpdated(const OrderBook& book);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);

private slots:
    // Timer-based order book capture for liquidity time series
    void captureOrderBookSnapshot();

signals:
    void renderModeChanged();
    void showVolumeProfileChanged();
    void intensityScaleChanged();
    void maxCellsChanged();
    void gridResolutionChanged(int timeRes_ms, double priceRes);
    void autoScrollEnabledChanged();
    void minVolumeFilterChanged();
    void timeframeChanged();

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
    
    // 🚀 OPTIMIZED RENDERING METHODS (cache + transform)
    void updateCachedGeometry();  // Only when data changes
    bool shouldRefreshCache() const;
    QMatrix4x4 calculateViewportTransform() const;
    void applyTransformToCache(const QMatrix4x4& transform);
    
    // Legacy methods (for data updates)
    void updateVisibleCells();
    void updateVolumeProfile();
    QSGNode* createHeatmapNode(const std::vector<CellInstance>& cells);
    QSGNode* createTradeFlowNode(const std::vector<CellInstance>& cells);
    QSGNode* createVolumeProfileNode(const std::vector<std::pair<double, double>>& profile);
    QSGNode* createCandleNode(const std::vector<CellInstance>& cells);
    
    // Color calculation for different modes
    QColor calculateHeatmapColor(double liquidity, bool isBid, double intensity) const;
    QColor calculateTradeFlowColor(double liquidity, bool isBid, double intensity) const;
    QColor calculateCandleColor(double liquidity, bool isBid, double intensity) const;
    
    // Utility methods
    void ensureGeometryCapacity(QSGGeometryNode* node, int vertexCount);
    double calculateIntensity(double liquidity) const;
    
    // Liquidity time series integration
    void createCellsFromLiquiditySlice(const LiquidityTimeSlice& slice);
    void createLiquidityCell(const LiquidityTimeSlice& slice, double price, double liquidity, bool isBid);
    QRectF timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const;
    
    // 🚀 OPTIMIZED CACHE METHODS
    void createCachedLiquidityCell(const LiquidityTimeSlice& slice, double price, double liquidity, bool isBid);
    QRectF timeSliceToCacheRect(const LiquidityTimeSlice& slice, double price) const;
    
    // Viewport management helpers
    void updateViewportFromZoom();
    void updateViewportFromPan();
    
    // Viewport and order book state
    int64_t m_visibleTimeStart_ms = 0;
    int64_t m_visibleTimeEnd_ms = 0;
    double m_minPrice = 0.0;
    double m_maxPrice = 0.0;
    bool m_timeWindowValid = false;
    OrderBook m_latestOrderBook;
    bool m_hasValidOrderBook = false;
    
    // Pan/zoom state
    bool m_autoScrollEnabled = true;
    double m_zoomFactor = 1.0;
    double m_panOffsetTime_ms = 0.0;
    double m_panOffsetPrice = 0.0;
    
    // Mouse interaction state
    bool m_isDragging = false;
    QPointF m_lastMousePos;
    QPointF m_initialMousePos;
    
    // 🚀 OPTIMIZATION: Separate visual pan from data pan
    QPointF m_panVisualOffset; // Temporary visual-only offset during a mouse drag
    QElapsedTimer m_interactionTimer;
};