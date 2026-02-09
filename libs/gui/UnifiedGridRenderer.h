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
#include <QSizeF>
#include <QColor>
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <limits>
#include "../core/config/ConfigTypes.hpp"
#include "../core/marketdata/model/TradeData.h"
#include "render/GridViewState.hpp"
#include "render/MsdfAtlas.hpp"
#include "render/HeatmapLabelRenderer.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/TimeAxisMapping.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
#include "render/FootprintOverlayRenderer.hpp"

class DataProcessor;
class MsdfGlyphNode;

struct ColorStop {
    float position;
    QColor color;

    ColorStop(float pos, const QColor& col) : position(pos), color(col) {}
};

struct ColorGradient {
    std::vector<ColorStop> stops;

    ColorGradient() = default;
    ColorGradient(std::initializer_list<ColorStop> stopList) : stops(stopList) {}

    QColor interpolate(float t) const {
        if (stops.empty()) return QColor(0, 0, 0);
        if (stops.size() == 1 || t <= stops.front().position) return stops.front().color;
        if (t >= stops.back().position) return stops.back().color;

        for (size_t i = 0; i < stops.size() - 1; ++i) {
            if (t >= stops[i].position && t <= stops[i + 1].position) {
                const float localT = (t - stops[i].position) / (stops[i + 1].position - stops[i].position);
                const QColor& c0 = stops[i].color;
                const QColor& c1 = stops[i + 1].color;
                return QColor(
                    c0.red() + (c1.red() - c0.red()) * localT,
                    c0.green() + (c1.green() - c0.green()) * localT,
                    c0.blue() + (c1.blue() - c0.blue()) * localT
                );
            }
        }
        return stops.back().color;
    }
};

class UnifiedGridRenderer : public QQuickItem, public ITimeAxisMappingProvider {
    Q_OBJECT
    Q_INTERFACES(ITimeAxisMappingProvider)
    QML_ELEMENT
    
    Q_PROPERTY(double intensityScale READ intensityScale WRITE setIntensityScale NOTIFY intensityScaleChanged)
    Q_PROPERTY(int maxCells READ maxCells WRITE setMaxCells NOTIFY maxCellsChanged)
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
    Q_PROPERTY(double minVolumeFilter READ minVolumeFilter WRITE setMinVolumeFilter NOTIFY minVolumeFilterChanged)
    Q_PROPERTY(double currentPriceResolution READ getCurrentPriceResolution NOTIFY priceResolutionChanged)
    Q_PROPERTY(double autoScrollPaddingFrac READ autoScrollPaddingFrac WRITE setAutoScrollPaddingFrac NOTIFY autoScrollPaddingFracChanged)
    Q_PROPERTY(bool autoScrollSmoothEnabled READ autoScrollSmoothEnabled WRITE setAutoScrollSmoothEnabled NOTIFY autoScrollSmoothEnabledChanged)
    Q_PROPERTY(QColor heatmapBackgroundColor READ heatmapBackgroundColor WRITE setHeatmapBackgroundColor NOTIFY heatmapBackgroundColorChanged)
    Q_PROPERTY(double heatmapGamma READ heatmapGamma WRITE setHeatmapGamma NOTIFY heatmapGammaChanged)
    Q_PROPERTY(double heatmapContrast READ heatmapContrast WRITE setHeatmapContrast NOTIFY heatmapContrastChanged)
    Q_PROPERTY(double heatmapShaderFloor READ heatmapShaderFloor WRITE setHeatmapShaderFloor NOTIFY heatmapShaderFloorChanged)
    Q_PROPERTY(int primaryField READ primaryField NOTIFY primaryFieldChanged)
    
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
    Q_PROPERTY(double heatmapTickSize READ heatmapTickSize NOTIFY heatmapTickSizeChanged)

    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)

    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)
    Q_PROPERTY(int liquidityLabelMode READ liquidityLabelMode WRITE setLiquidityLabelMode NOTIFY liquidityLabelModeChanged)
    Q_PROPERTY(double heatmapLiquidityThreshold READ heatmapLiquidityThreshold WRITE setHeatmapLiquidityThreshold NOTIFY heatmapLiquidityThresholdChanged)
    Q_PROPERTY(QObject* viewState READ viewState CONSTANT)

private:
    struct FrameViewportSnapshot {
        bool valid = false;
        qint64 timeStart = 0;
        qint64 timeEnd = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        QPointF panVisualOffset;
        bool dragging = false;
        bool autoScrollEnabled = false;
    };

    struct FrameStreamGenerations {
        uint64_t heatmap = 0;
        uint64_t footprint = 0;
        uint64_t candle = 0;
    };

    struct FrameContext {
        struct OverlayActivationSet {
            bool heatmap = false;
            bool footprint = false;
        };
        QRectF surfaceBounds;
        double surfaceDpr = 1.0;
        qint64 presentationTimeMs = 0;
        FrameViewportSnapshot viewport;
        HeatmapStreamState::Snapshot heatmapSnapshot;
        FrameStreamGenerations streamGenerations;
        OverlayActivationSet overlays;
        bool forceFull = false;
        TimeAxisMapping mapping;
    };

    double m_intensityScale = 1.0;
    int m_maxCells = 100000;
    double m_minVolumeFilter = 0.0;
    int64_t m_currentTimeframe_ms = 100;
    
    bool m_showGpuStatsOverlay = false;
    bool m_showDataPipelineOverlay = false;
    bool m_showRenderStrategyOverlay = false;
    bool m_showViewportMathOverlay = false;
    bool m_showMemoryCacheOverlay = false;
    bool m_showModeFlagsOverlay = false;
    int m_liquidityLabelMode = 0;
    double m_heatmapLiquidityThreshold = 0.0;
    double m_heatmapTickSize = 0.0;

    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    
    bool m_panSyncPending = false;

    bool m_useGpuHeatmap = false;
    int m_heatmapGridWidth = 5120;
    int m_heatmapGridHeight = 2048;
    // Cross-thread flag: set on main thread, consumed/reset on render thread.
    std::atomic<bool> m_heatmapTextureDirty{true};
    QImage m_heatmapImage;
    QImage m_heatmapPaletteImage;
    QTimer* m_heatmapRenderTimer = nullptr;
    bool m_heatmapViewportInitialized = false;
    int m_intensityBytesPerCell = 1;
    QElapsedTimer m_heatmapClock;
    std::unique_ptr<class HeatmapStreamState> m_heatmapStream;
    std::unique_ptr<class ViewportAutoScrollController> m_autoScrollController;
    double m_autoScrollPaddingFrac = 0.08;
    bool m_smoothAutoScrollEnabled = true;
    QColor m_heatmapBackgroundColor = QColor(18, 20, 24);
    double m_heatmapGamma = 1.05;
    double m_heatmapContrast = 1.15;
    double m_heatmapShaderFloor = 0.01;
    int m_heatmapLabelPx = 14;
    int m_primaryField = 0;

    double m_heatmapPaletteGamma = 2.0;
    ColorGradient m_bidGradient;
    ColorGradient m_askGradient;
    bool m_heatmapPaletteDirty = true;
    TimeAxisMapping m_lastTimeAxisMapping;
    mutable std::mutex m_frameContextMutex;
    MappingFrameContext m_lastFrameContext;

    MsdfAtlas m_msdfAtlas;
    bool m_msdfAtlasBuilt = false;
    int m_labelRingGridWidth = 0;
    int m_labelRingGridHeight = 0;
    std::vector<uint16_t> m_labelLiquidityRing;
    std::vector<uint16_t> m_labelIntensityRing;
    std::vector<double> m_labelLiquidityScales;
    std::vector<HeatmapLabelRenderer::GlyphQuad> m_labelWhiteQuads;
    std::vector<HeatmapLabelRenderer::GlyphQuad> m_labelBlackQuads;
    class MsdfGlyphNode* m_whiteGlyphNode = nullptr;
    class MsdfGlyphNode* m_blackGlyphNode = nullptr;
    FootprintOverlayRenderer m_footprintOverlay;
    // Main thread enqueues pending uploads; render thread swaps to local frame snapshot.
    mutable std::mutex m_footprintPendingMutex;
    std::vector<FootprintOverlayRenderer::PendingUpload> m_pendingFootprintUploads;

    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount = 0;
    std::atomic<double> m_currentFps{0.0};

    QElapsedTimer m_uploadTimer;
    std::atomic<qint64> m_totalBytesUploaded{0};
    std::atomic<double> m_uploadBandwidthMBps{0.0};
    qint64 m_lastBandwidthUpdate = 0;
    std::atomic<uint64_t> m_heatmapStreamGeneration{0};
    std::atomic<uint64_t> m_footprintStreamGeneration{0};
    std::atomic<uint64_t> m_candleStreamGeneration{0};

public:
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer();
    
    double intensityScale() const { return m_intensityScale; }
    int maxCells() const { return m_maxCells; }
    int64_t currentTimeframe() const { return m_currentTimeframe_ms; }
    double minVolumeFilter() const { return m_minVolumeFilter; }
    bool autoScrollEnabled() const { return m_viewState ? m_viewState->isAutoScrollEnabled() : false; }
    double autoScrollPaddingFrac() const { return m_autoScrollPaddingFrac; }
    bool autoScrollSmoothEnabled() const { return m_smoothAutoScrollEnabled; }
    int liquidityLabelMode() const { return m_liquidityLabelMode; }
    double heatmapLiquidityThreshold() const { return m_heatmapLiquidityThreshold; }
    QColor heatmapBackgroundColor() const { return m_heatmapBackgroundColor; }
    double heatmapGamma() const { return m_heatmapGamma; }
    double heatmapContrast() const { return m_heatmapContrast; }
    double heatmapShaderFloor() const { return m_heatmapShaderFloor; }
    int primaryField() const { return m_primaryField; }
    
    GridViewState* getViewState() const { return m_viewState.get(); }
    QObject* viewState() const { return m_viewState.get(); }
    
    bool showGpuStatsOverlay() const { return m_showGpuStatsOverlay; }
    bool showDataPipelineOverlay() const { return m_showDataPipelineOverlay; }
    bool showRenderStrategyOverlay() const { return m_showRenderStrategyOverlay; }
    bool showViewportMathOverlay() const { return m_showViewportMathOverlay; }
    bool showMemoryCacheOverlay() const { return m_showMemoryCacheOverlay; }
    bool showModeFlagsOverlay() const { return m_showModeFlagsOverlay; }

    Q_INVOKABLE qint64 getVisibleTimeStart() const;
    Q_INVOKABLE qint64 getVisibleTimeEnd() const; 
    Q_INVOKABLE double getMinPrice() const;
    Q_INVOKABLE double getMaxPrice() const;
    
    int getCurrentTimeframe() const { return static_cast<int>(m_currentTimeframe_ms); }
    
    Q_INVOKABLE QPointF getPanVisualOffset() const;
    double heatmapTickSize() const { return m_heatmapTickSize; }

    bool heatmapDataPriceRange(double& outMin, double& outMax) const;
    bool heatmapDataTimeRange(qint64& outStart, qint64& outEnd) const;
    
    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    
    Q_INVOKABLE void setPriceResolution(double resolution);
    Q_INVOKABLE int getCurrentTimeResolution() const;
    Q_INVOKABLE double getCurrentPriceResolution() const;
    Q_INVOKABLE void setGridResolution(int timeResMs, double priceRes);
    Q_INVOKABLE void fitHeatmapToDataRange();
    struct GridResolution {
        int timeMs;
        double price;
    };
    static GridResolution calculateOptimalResolution(qint64 timeSpanMs, double priceSpan, int targetVerticalLines = 10, int targetHorizontalLines = 15);
    
    Q_INVOKABLE QString getGridDebugInfo() const;
    Q_INVOKABLE QString getDetailedGridDebug() const;
    Q_INVOKABLE QString getViewportMathDebug() const;
    Q_INVOKABLE QString getDataPipelineDebug() const;
    
    Q_INVOKABLE void togglePerformanceOverlay();
    Q_INVOKABLE QString getPerformanceStats() const;
    Q_INVOKABLE double getCurrentFPS() const;
    Q_INVOKABLE double getAverageRenderTime() const;
    Q_INVOKABLE double getCacheHitRate() const;

    Q_INVOKABLE QString getTextureSize() const;
    Q_INVOKABLE QString getTextureMemory() const;
    Q_INVOKABLE QString getTextureFormat() const;
    Q_INVOKABLE double getUploadBandwidth() const;
    Q_INVOKABLE QString getRingCursorInfo() const;
    Q_INVOKABLE int getDirtyRegionCount() const;
    Q_INVOKABLE QString getLabelRingMemory() const;
    Q_INVOKABLE QString getMsdfAtlasMemory() const;
    TimeAxisMapping lastTimeAxisMapping() const { return currentTimeAxisMapping(); }
    MappingFrameContext currentFrameContext() const override;
    TimeAxisMapping currentTimeAxisMapping() const override;
    void applyClientConfig(const ClientConfig& config);
    void applyServerConfig(const ServerConfig& config);

    Q_INVOKABLE void setGridResolutionPreset(int preset);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    Q_INVOKABLE void setLiquidityLabelMode(int mode);
    
    
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
    
    Q_INVOKABLE QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    Q_INVOKABLE QPointF screenToWorld(double screenX, double screenY) const;
    Q_INVOKABLE double getScreenWidth() const;
    Q_INVOKABLE double getScreenHeight() const;
    Q_INVOKABLE double getZoomFactor() const;
    void setHeatmapGamma(double gamma);
    void setHeatmapContrast(double contrast);
    void setHeatmapShaderFloor(double floor);
    void setPrimaryField(int field);

public:
    void onTradeReceived(const Trade& trade);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    void onViewportChanged();

signals:
    void intensityScaleChanged();
    void maxCellsChanged();
    void gridResolutionChanged(int timeRes_ms, double priceRes);
    void autoScrollEnabledChanged();
    void minVolumeFilterChanged();
    void priceResolutionChanged();
    void autoScrollPaddingFracChanged();
    void autoScrollSmoothEnabledChanged();
    void showGpuStatsOverlayChanged();
    void showDataPipelineOverlayChanged();
    void showRenderStrategyOverlayChanged();
    void showViewportMathOverlayChanged();
    void showMemoryCacheOverlayChanged();
    void showModeFlagsOverlayChanged();
    void liquidityLabelModeChanged();
    void heatmapLiquidityThresholdChanged();
    void heatmapBackgroundColorChanged();
    void heatmapGammaChanged();
    void heatmapContrastChanged();
    void heatmapShaderFloorChanged();
    void primaryFieldChanged();
    void viewportChanged();
    void timeframeChanged();
    void panVisualOffsetChanged();
    void heatmapTickSizeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void ensureHeatmapImage();
    void ensureHeatmapPaletteImage();
    void buildMsdfAtlas();
    void applyLabelUploads(const std::vector<HeatmapStreamState::PendingLabelColumn>& uploads,
                           int gridWidth,
                           int gridHeight);

private:
    void setIntensityScale(double scale);
    void setMaxCells(int max);
    void setMinVolumeFilter(double minVolume);
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowRenderStrategyOverlay(bool show);
    void setShowViewportMathOverlay(bool show);
    void setShowMemoryCacheOverlay(bool show);
    void setShowModeFlagsOverlay(bool show);
    void setAutoScrollPaddingFrac(double fraction);
    void setAutoScrollSmoothEnabled(bool enabled);
    void setHeatmapLiquidityThreshold(double threshold);
    void setHeatmapBackgroundColor(const QColor& color);

    std::unique_ptr<GridViewState> m_viewState;
    std::unique_ptr<DataProcessor> m_dataProcessor;
    std::unique_ptr<QThread> m_dataProcessorThread;
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_dataProcessor.get(); }
};
