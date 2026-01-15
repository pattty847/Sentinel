// ======= libs/gui/UnifiedGridRenderer.h =======
/*
Sentinel — UnifiedGridRenderer
Role: A QML scene item that renders the GPU-only heatmap path.
Inputs/Outputs: Takes heatmap columns via DataProcessor; outputs a QSGNode for GPU rendering.
Threading: Runs on the GUI thread; data is received via queued connections; rendering on render thread.
Performance: Batches incoming data with a QTimer to throttle scene graph updates.
Integration: Used in QML; receives data from MarketDataCore and server heatmap stream.
Observability: Logs key events like data reception and paint node updates via qDebug.
Related: UnifiedGridRenderer.cpp, CoordinateSystem.h, MarketDataCore.hpp.
Assumptions: CoordinateSystem and ChartModeController properties are set from QML.
*/
#pragma once
#include <QQuickItem>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QImage>
#include <QTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QByteArray>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include "../core/marketdata/model/TradeData.h"
#include "render/GridViewState.hpp"

class DataProcessor;

/**
 *  UNIFIED GRID RENDERER - SLIM QML ADAPTER
 * 
 * Slim QML adapter that delegates to the V2 modular architecture.
 * Maintains QML interface compatibility while using the new modular system.
 */
class UnifiedGridRenderer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(double intensityScale READ intensityScale WRITE setIntensityScale NOTIFY intensityScaleChanged)
    Q_PROPERTY(int maxCells READ maxCells WRITE setMaxCells NOTIFY maxCellsChanged)
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
    Q_PROPERTY(double minVolumeFilter READ minVolumeFilter WRITE setMinVolumeFilter NOTIFY minVolumeFilterChanged)
    Q_PROPERTY(double currentPriceResolution READ getCurrentPriceResolution NOTIFY priceResolutionChanged)
    
    // Debug Overlay Toggles
    Q_PROPERTY(bool showGpuStatsOverlay READ showGpuStatsOverlay WRITE setShowGpuStatsOverlay NOTIFY showGpuStatsOverlayChanged)
    Q_PROPERTY(bool showDataPipelineOverlay READ showDataPipelineOverlay WRITE setShowDataPipelineOverlay NOTIFY showDataPipelineOverlayChanged)
    Q_PROPERTY(bool showRenderStrategyOverlay READ showRenderStrategyOverlay WRITE setShowRenderStrategyOverlay NOTIFY showRenderStrategyOverlayChanged)
    Q_PROPERTY(bool showViewportMathOverlay READ showViewportMathOverlay WRITE setShowViewportMathOverlay NOTIFY showViewportMathOverlayChanged)
    Q_PROPERTY(bool showMemoryCacheOverlay READ showMemoryCacheOverlay WRITE setShowMemoryCacheOverlay NOTIFY showMemoryCacheOverlayChanged)
    Q_PROPERTY(bool showModeFlagsOverlay READ showModeFlagsOverlay WRITE setShowModeFlagsOverlay NOTIFY showModeFlagsOverlayChanged)

    Q_PROPERTY(qint64 visibleTimeStart READ getVisibleTimeStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 visibleTimeEnd READ getVisibleTimeEnd NOTIFY viewportChanged)
    Q_PROPERTY(double minPrice READ getMinPrice NOTIFY viewportChanged)
    Q_PROPERTY(double maxPrice READ getMaxPrice NOTIFY viewportChanged)

    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)

    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)

private:
    // Rendering configuration
    double m_intensityScale = 1.0;
    int m_maxCells = 100000;
    double m_minVolumeFilter = 0.0;      // Volume filter
    int64_t m_currentTimeframe_ms = 100;  // Default to 100ms for smooth updates
    
    // Debug overlay toggles
    bool m_showGpuStatsOverlay = false;
    bool m_showDataPipelineOverlay = false;
    bool m_showRenderStrategyOverlay = false;
    bool m_showViewportMathOverlay = false;
    bool m_showMemoryCacheOverlay = false;
    bool m_showModeFlagsOverlay = false;

    bool m_manualTimeframeSet = false;  // Disable auto-suggestion when user manually sets timeframe
    QElapsedTimer m_manualTimeframeTimer;  // Reset auto-suggestion after delay
    
    bool m_panSyncPending = false;  // hold visual pan until DP resync snapshot applied to avoid snap-back -- TODO: See if this is needed

    // Dummy GPU heatmap path for testing single-quad rendering.
    bool m_useDummyHeatmap = false;
    int m_dummyGridSize = 2048;
    bool m_dummyTextureDirty = true;
    QImage m_dummyImage;
    QImage m_dummyPaletteImage;
    QTimer* m_dummyAppendTimer = nullptr;
    QTimer* m_dummyRenderTimer = nullptr;
    int m_dummyWriteColumn = 0;
    int64_t m_dummyTimeIndex = 0;
    int m_dummyAppendMs = 100;
    qint64 m_dummyLastAppendMs = 0;
    QElapsedTimer m_dummyClock;
    std::atomic<float> m_dummyTimeOffset{0.0f};
    bool m_dummyLabelDirty = true;
    QRectF m_dummyLabelSourceRect;
    QSize m_dummyLabelPixelSize;
    int m_dummyLabelStartX = 0;
    std::mutex m_dummyImageMutex;
    std::mutex m_dummyLabelMutex;
    QImage m_dummyLabelImage;
    std::atomic<int> m_dummyLabelVersion{0};
    int m_dummyLabelTextureVersion = -1;
    struct DummyLabelRequest {
        QRectF srcRect;
        QSize labelSize;
        int startX = 0;
        int startY = 0;
        float cellW = 0.0f;
        float cellH = 0.0f;
        bool valid = false;
    };
    std::mutex m_dummyLabelRequestMutex;
    DummyLabelRequest m_dummyLabelRequest;
    struct DummyPendingColumn {
        int x = 0;
        QByteArray data;
    };
    mutable std::mutex m_dummyUploadMutex;
    std::vector<DummyPendingColumn> m_dummyPendingColumns;

    // Real GPU heatmap path (single quad with streamed columns).
    bool m_useGpuHeatmap = false;
    int m_heatmapGridSize = 8192;
    bool m_heatmapTextureDirty = true;
    QImage m_heatmapImage;
    QImage m_heatmapPaletteImage;
    QTimer* m_heatmapRenderTimer = nullptr;
    int m_heatmapWriteColumn = 0;
    int m_heatmapAppendMs = 100;
    qint64 m_heatmapLastAppendMs = 0;
    int64_t m_heatmapTimeOriginMs = 0;
    int64_t m_heatmapLastSliceStartMs = std::numeric_limits<int64_t>::min();
    bool m_heatmapViewportInitialized = false;
    double m_heatmapMinPrice = 0.0;
    double m_heatmapMaxPrice = 0.0;
    double m_heatmapTickSize = 0.0;
    QElapsedTimer m_heatmapClock;
    std::atomic<float> m_heatmapTimeOffset{0.0f};
    QByteArray m_heatmapLastColumnData;
    bool m_heatmapHaveLastColumn = false;
    int64_t m_autoScrollLagMs = 0;
    int64_t m_autoScrollSpanMs = 0;
    double m_autoScrollPaddingFrac = 0.05;
    struct HeatmapPendingColumn {
        int x = 0;
        QByteArray data;
    };
    mutable std::mutex m_heatmapUploadMutex;
    std::vector<HeatmapPendingColumn> m_heatmapPendingColumns;

    // FPS tracking (updated on render thread, read on GUI thread).
    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount = 0;
    std::atomic<double> m_currentFps{0.0};

    // GPU upload bandwidth tracking
    QElapsedTimer m_uploadTimer;
    std::atomic<qint64> m_totalBytesUploaded{0};
    std::atomic<double> m_uploadBandwidthMBps{0.0};
    qint64 m_lastBandwidthUpdate = 0;

    // V1 state (removed - now delegated to DataProcessor)

public:
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer(); // Custom destructor needed for unique_ptr with incomplete types
    
    // Property accessors
    double intensityScale() const { return m_intensityScale; }
    int maxCells() const { return m_maxCells; }
    int64_t currentTimeframe() const { return m_currentTimeframe_ms; }
    double minVolumeFilter() const { return m_minVolumeFilter; }
    bool autoScrollEnabled() const { return m_viewState ? m_viewState->isAutoScrollEnabled() : false; }
    
    // GridViewState accessor (for axis models)
    GridViewState* getViewState() const { return m_viewState.get(); }
    
    // Debug overlay accessors
    bool showGpuStatsOverlay() const { return m_showGpuStatsOverlay; }
    bool showDataPipelineOverlay() const { return m_showDataPipelineOverlay; }
    bool showRenderStrategyOverlay() const { return m_showRenderStrategyOverlay; }
    bool showViewportMathOverlay() const { return m_showViewportMathOverlay; }
    bool showMemoryCacheOverlay() const { return m_showMemoryCacheOverlay; }
    bool showModeFlagsOverlay() const { return m_showModeFlagsOverlay; }

    //  VIEWPORT BOUNDS: Getters for QML properties
    Q_INVOKABLE qint64 getVisibleTimeStart() const;
    Q_INVOKABLE qint64 getVisibleTimeEnd() const; 
    Q_INVOKABLE double getMinPrice() const;
    Q_INVOKABLE double getMaxPrice() const;
    
    //  OPTIMIZATION 4: QML-compatible timeframe getter (returns int for Q_PROPERTY)
    int getCurrentTimeframe() const { return static_cast<int>(m_currentTimeframe_ms); }
    
    //  VISUAL TRANSFORM: Getter for QML pan offset
    Q_INVOKABLE QPointF getPanVisualOffset() const;
    
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

    //  GPU STATS DEBUG API
    Q_INVOKABLE QString getTextureSize() const;          // e.g., "8192x8192"
    Q_INVOKABLE QString getTextureMemory() const;        // e.g., "128 MB"
    Q_INVOKABLE QString getTextureFormat() const;        // e.g., "RGBA8" or "R16"
    Q_INVOKABLE double getUploadBandwidth() const;       // MB/s
    Q_INVOKABLE QString getRingCursorInfo() const;       // e.g., "4256/8192"
    Q_INVOKABLE int getDirtyRegionCount() const;         // Number of dirty tiles/regions

    //  GRID SYSTEM CONTROLS
    Q_INVOKABLE void setGridMode(int mode);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    void setDataSource(class IGridDataSource* source); // Forward declaration - implemented in .cpp
    
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
    Q_INVOKABLE double getZoomFactor() const;

public:
    // Real-time data integration
    void onTradeReceived(const Trade& trade);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    
    // Automatic price resolution adjustment on viewport changes
    void onViewportChanged();

signals:
    void intensityScaleChanged();
    void maxCellsChanged();
    void gridResolutionChanged(int timeRes_ms, double priceRes);
    void autoScrollEnabledChanged();
    void minVolumeFilterChanged();
    void priceResolutionChanged();
    void showGpuStatsOverlayChanged();
    void showDataPipelineOverlayChanged();
    void showRenderStrategyOverlayChanged();
    void showViewportMathOverlayChanged();
    void showMemoryCacheOverlayChanged();
    void showModeFlagsOverlayChanged();
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
    void initDummyHeatmap();
    void ensureDummyImage();
    void ensureDummyPaletteImage();
    void ensureHeatmapImage();
    void ensureHeatmapPaletteImage();
    void appendDummyColumn();
    void onDummyRenderTick();
    void buildDummyLabelImage();

private:
    // Property setters
    void setIntensityScale(double scale);
    void setMaxCells(int max);
    void setMinVolumeFilter(double minVolume);
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowRenderStrategyOverlay(bool show);
    void setShowViewportMathOverlay(bool show);
    void setShowMemoryCacheOverlay(bool show);
    void setShowModeFlagsOverlay(bool show);
    class IGridDataSource* m_dataSource = nullptr;

    std::unique_ptr<GridViewState> m_viewState;
    std::unique_ptr<DataProcessor> m_dataProcessor;
    std::unique_ptr<QThread> m_dataProcessorThread;
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_dataProcessor.get(); }
};
