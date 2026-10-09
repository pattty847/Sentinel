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
#include <QColor>
#include <QPointer>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include "../core/config/ConfigTypes.hpp"
#include "../core/marketdata/model/TradeData.h"
#include "datasources/CandleSeriesBuffer.hpp"
// ── Core rendering types (needed by inline members) ──────────────────────────
#include "render/GridViewState.hpp"
#include "render/AxisLayout.hpp"
#include "render/ChartTextAtlas.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/heatmap/HeatmapLabelLayout.hpp"
#include "render/TimeAxisMapping.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
// ── Extracted services ───────────────────────────────────────────────────────
#include "render/AxisTextService.hpp"
#include "render/FrameProfiler.hpp"
#include "render/FrameContext.hpp"
#include "render/TimeAuthority.hpp"
// ── Overlay renderers (owned inline) ─────────────────────────────────────────
#include "render/IOverlayRenderer.hpp"
#include "render/FootprintOverlayRenderer.hpp"
#include "render/TradeBubbleFrame.hpp"
#include "render/TpoOverlayRenderer.hpp"
#include "render/VolumeProfileRenderer.hpp"

class DataProcessor;
namespace heatmap {
class HeatmapDataService;
class ManualTickMemory;
struct HeatmapChartSettings;
}
namespace heatmap::gpu {
class HeatmapGpuLayer;
class HeatmapTileNode;
struct ViewWindow;
}
class QQuickWindow;
class QScreen;

class UnifiedGridRenderer : public QQuickItem, public ITimeAxisMappingProvider {
    friend struct TradeBubbleRendererTest;
    Q_OBJECT
    Q_INTERFACES(ITimeAxisMappingProvider)
    QML_ELEMENT
    
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
    Q_PROPERTY(double minVolumeFilter READ minVolumeFilter WRITE setMinVolumeFilter NOTIFY minVolumeFilterChanged)
    Q_PROPERTY(double currentPriceResolution READ getCurrentPriceResolution NOTIFY priceResolutionChanged)
    Q_PROPERTY(double autoScrollPaddingFrac READ autoScrollPaddingFrac WRITE setAutoScrollPaddingFrac NOTIFY autoScrollPaddingFracChanged)
    Q_PROPERTY(bool autoScrollSmoothEnabled READ autoScrollSmoothEnabled WRITE setAutoScrollSmoothEnabled NOTIFY autoScrollSmoothEnabledChanged)
    Q_PROPERTY(double heatmapGamma READ heatmapGamma WRITE setHeatmapGamma NOTIFY heatmapGammaChanged)
    Q_PROPERTY(double heatmapContrast READ heatmapContrast WRITE setHeatmapContrast NOTIFY heatmapContrastChanged)
    Q_PROPERTY(double heatmapShaderFloor READ heatmapShaderFloor WRITE setHeatmapShaderFloor NOTIFY heatmapShaderFloorChanged)
    Q_PROPERTY(int primaryField READ primaryField NOTIFY primaryFieldChanged)
    Q_PROPERTY(bool heatmapLayerEnabled READ heatmapLayerEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool footprintLayerEnabled READ footprintLayerEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool tpoLayerEnabled READ tpoLayerEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(bool volumeProfileLayerEnabled READ volumeProfileLayerEnabled NOTIFY layerVisibilityChanged)
    Q_PROPERTY(int tpoTimeframeMs READ tpoTimeframeMs WRITE setTpoTimeframeMs NOTIFY tpoConfigChanged)
    Q_PROPERTY(int tpoSessionType READ tpoSessionType WRITE setTpoSessionType NOTIFY tpoConfigChanged)
    
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
    Q_PROPERTY(bool tradesAboveCandles READ tradesAboveCandles NOTIFY tradeBubbleSettingsChanged)

    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)

    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)
    Q_PROPERTY(int candleStyle READ candleStyle WRITE setCandleStyle NOTIFY candleStyleChanged)
    Q_PROPERTY(QColor candleUpColor READ candleUpColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QColor candleDownColor READ candleDownColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QString candleWickColor READ candleWickColor NOTIFY candleAppearanceChanged)
    Q_PROPERTY(double candleBodyOpacity READ candleBodyOpacity NOTIFY candleAppearanceChanged)
    Q_PROPERTY(int candleWickWidth READ candleWickWidth NOTIFY candleAppearanceChanged)
    Q_PROPERTY(QObject* viewState READ viewState CONSTANT)
    Q_PROPERTY(QObject* priceAxisSource READ priceAxisSource WRITE setPriceAxisSource NOTIFY axisSourcesChanged)
    Q_PROPERTY(QObject* timeAxisSource READ timeAxisSource WRITE setTimeAxisSource NOTIFY axisSourcesChanged)
    Q_PROPERTY(double effectiveAxisLabelPx READ effectiveAxisLabelPx NOTIFY axisLayoutChanged)
    Q_PROPERTY(int priceAxisWidthPx READ priceAxisWidthPx NOTIFY axisLayoutChanged)
    Q_PROPERTY(int timeAxisHeightPx READ timeAxisHeightPx NOTIFY axisLayoutChanged)
    // The chart's candle series (auto-fit reads the visible candles' high/low).
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer WRITE setCandleBuffer NOTIFY candleBufferChanged)
    Q_PROPERTY(bool autoPriceScale READ autoPriceScale WRITE setAutoPriceScale NOTIFY autoPriceScaleChanged)

private:
    double m_minVolumeFilter = 0.0;
    int64_t m_currentTimeframe_ms = 100;
    
    bool m_showGpuStatsOverlay = false;
    bool m_showDataPipelineOverlay = false;
    bool m_showRenderStrategyOverlay = false;
    bool m_showViewportMathOverlay = false;
    bool m_showMemoryCacheOverlay = false;
    bool m_showModeFlagsOverlay = false;
    int m_candleStyle = 0;
    QColor m_candleUpColor{"#2EBD85"}, m_candleDownColor{"#F6465D"};
    QString m_candleWickColor = "auto"; // each wick follows its body
    double m_candleBodyOpacity = 1;
    int m_candleWickWidth = 1;

    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;

    FrameProfiler m_frameProfiler;  // SENTINEL_FRAME_PROFILE=1, render thread only
    QString m_activeSymbol;

    // S6b GPU heatmap (the only heatmap renderer since S8a). GUI thread state,
    // read in updatePaintNode (GUI blocked).
    std::unique_ptr<heatmap::gpu::HeatmapGpuLayer> m_gpuLayer;
    bool m_gpuPriceKnown = false;   // the viewport's price window is real (not a placeholder)
    bool m_gpuReseedPrice = false;  // a symbol switch: the next book top centres price
    // The seeded time window follows the chart's size until the user (or the
    // Agent API) moves the view: the first book top can arrive before layout.
    bool m_gpuViewPristine = false;
    bool m_gpuSelfViewport = false; // the seed or follow-live is setting the viewport
    bool m_gpuLimitsDeferred = false; // a timeframe switch applies limits and view as one change
    // The chart's frame clock and timeframe cadence (FrameContext::time; the candle
    // overlay's boundary sequence reads it through currentFrameContext()).
    QElapsedTimer m_frameClock;
    TimeAuthority m_timeAuthority;
    // The first view (heatmap.initial_column_px / initial_price_pct): the time span is
    // initial_column_px per column (16 columns min); the price window is
    // initial_price_pct of kSeedBandRows rows at kSeedBandTick (the near layer's $1).
    int m_initialColumnPx = 8;
    int m_initialPricePct = 5;
    static constexpr double kSeedBandTick = 1.0;
    static constexpr int kSeedBandRows = 2048;
    double m_autoScrollPaddingFrac = 0.08;
    bool m_smoothAutoScrollEnabled = true;
    double m_heatmapGamma = 1.05;
    double m_heatmapContrast = 1.15;
    double m_heatmapShaderFloor = 0.01;
    int m_primaryField = 0;
    bool m_heatmapLayerEnabled = true;
    bool m_footprintLayerEnabled = false;
    bool m_tpoLayerEnabled = false;
    bool m_volumeProfileLayerEnabled = false;

    mutable std::mutex m_frameContextMutex;
    MappingFrameContext m_lastFrameContext;
    // Whole-pixel mapping: last frame's drawn pixels per row/column (the step rule's
    // hysteresis state). Written by the render thread (GUI blocked), read by the GUI
    // thread's rasterCameraNow(); under m_frameContextMutex.
    chart_raster::RasterStep m_rasterStep;
    // Render thread: what rasterChanged() last announced.
    struct RasterSurface {
        chart_raster::RasterStep step;
        double dpr = 0;
        int widthDev = 0, heightDev = 0;
        bool operator==(const RasterSurface&) const = default;
    } m_rasterAnnounced;
    QMetaObject::Connection m_screenChangedConn;
    QMetaObject::Connection m_beforeSyncConn;
    // Render thread: this sync's frame (context, tick, raster camera), prepared in
    // beforeSynchronizing and drawn by updatePaintNode in the same sync.
    struct SyncFrame {
        FrameContext frame;
        bool drawHeatmap = false;
        bool ready = false;
    } m_sync;

    ChartTextAtlas m_chartTextAtlas;
    bool m_chartTextAtlasBuilt = false;
    ChartTextRenderer m_chartTextRenderer;
    std::vector<ChartGlyphInstance> m_heatmapLabelGlyphs;
    heatmap::gpu::HeatmapLabelLayout m_gpuLabels; // S7b: render thread (updatePaintNode)
    uint64_t m_gpuLabelSerial = 0;
    std::vector<uint8_t> m_gpuLabelColumns;          // render thread: matched label columns (reused)
    std::atomic<uint64_t> m_gpuLabelSignature{0};    // what the last frame's labels drew
    std::atomic<bool> m_gpuLabelsIncomplete{false};  // ...while some were held back (transition)
    std::unique_ptr<AxisTextService> m_axisTextService;
    std::shared_ptr<trade_bubbles::Tape> m_tradeBubbleTape = std::make_shared<trade_bubbles::Tape>();
    std::shared_ptr<trade_bubbles::RenderFrame> m_tradeBubbleFrame = std::make_shared<trade_bubbles::RenderFrame>(m_tradeBubbleTape);
    bool m_tradesAboveCandles = true;
    bool m_showTrades = false;
    double m_tradeMinNotional = 0;
    QColor m_tradeBuyColor, m_tradeSellColor;
    FootprintOverlayRenderer m_footprintOverlay;
    TpoOverlayRenderer m_tpoOverlay;
    VolumeProfileRenderer m_vpRenderer;
    std::vector<IOverlayRenderer*> m_overlays;  // non-owning; points to inline members above
    int m_tpoTimeframeMs = 1800000;           // 30m brackets (config tpo.period_minutes)
    int m_tpoSessionType = 4;                 // SessionManager::SessionType::H24

    std::atomic<uint64_t> m_footprintStreamGeneration{0};
    std::atomic<uint64_t> m_candleStreamGeneration{0};
    std::atomic<uint64_t> m_controlRevision{0};
    std::atomic<uint64_t> m_controlSelectionEpoch{0};
    std::atomic<uint64_t> m_controlViewportVersion{0};
    std::atomic<uint64_t> m_renderedRevision{0};
    std::atomic<uint64_t> m_renderedFrameId{0};
    uint64_t m_nextFrameId = 0; // render thread only
    uint64_t m_pendingFrameRevision = 0; // render thread only
    uint64_t m_pendingFrameId = 0; // render thread only

public:
    void setAgentControlRevision(uint64_t revision, uint64_t selectionEpoch, uint64_t viewportVersion) {
        m_controlSelectionEpoch.store(selectionEpoch, std::memory_order_release);
        m_controlViewportVersion.store(viewportVersion, std::memory_order_release);
        m_controlRevision.store(revision, std::memory_order_release);
        update();
    }
    uint64_t renderedControlRevision() const { return m_renderedRevision.load(std::memory_order_acquire); }
    uint64_t renderedFrameId() const { return m_renderedFrameId.load(std::memory_order_acquire); }
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer();
    
    int64_t currentTimeframe() const { return m_currentTimeframe_ms; }
    double minVolumeFilter() const { return m_minVolumeFilter; }
    bool autoScrollEnabled() const { return m_viewState ? m_viewState->isAutoScrollEnabled() : false; }
    double autoScrollPaddingFrac() const { return m_autoScrollPaddingFrac; }
    bool autoScrollSmoothEnabled() const { return m_smoothAutoScrollEnabled; }
    int candleStyle() const { return m_candleStyle; }
    void setCandleStyle(int style);
    QColor candleUpColor() const { return m_candleUpColor; }
    QColor candleDownColor() const { return m_candleDownColor; }
    QString candleWickColor() const { return m_candleWickColor; }
    double candleBodyOpacity() const { return m_candleBodyOpacity; }
    int candleWickWidth() const { return m_candleWickWidth; }
    void setCandleAppearance(const heatmap::HeatmapChartSettings& settings);
    double heatmapGamma() const { return m_heatmapGamma; }
    double heatmapContrast() const { return m_heatmapContrast; }
    double heatmapShaderFloor() const { return m_heatmapShaderFloor; }
    int primaryField() const { return m_primaryField; }
    bool heatmapLayerEnabled() const { return m_heatmapLayerEnabled; }
    bool footprintLayerEnabled() const { return m_footprintLayerEnabled; }
    bool tpoLayerEnabled() const { return m_tpoLayerEnabled; }
    bool volumeProfileLayerEnabled() const { return m_volumeProfileLayerEnabled; }
    int tpoTimeframeMs() const { return m_tpoTimeframeMs; }
    int tpoSessionType() const { return m_tpoSessionType; }
    Q_INVOKABLE void setVolumeProfileLayerEnabled(bool enabled);
    Q_INVOKABLE void setTpoTimeframeMs(int timeframeMs);
    Q_INVOKABLE void setTpoSessionType(int sessionType);
    // TPO look: layout "split"|"collapsed", theme "rainbow"|"calm"|"sage". Unknown names are ignored.
    Q_INVOKABLE void setTpoLayout(const QString& layout);
    Q_INVOKABLE void setTpoTheme(const QString& theme);
    QString tpoLayout() const;
    QString tpoTheme() const;
    int tpoSessions() const { return m_tpoOverlay.style().maxSessions; }
    
    GridViewState* getViewState() const { return m_viewState.get(); }
    QObject* viewState() const { return m_viewState.get(); }
    QObject* priceAxisSource() const { return m_axisTextService ? m_axisTextService->priceAxisSource() : nullptr; }
    QObject* timeAxisSource() const { return m_axisTextService ? m_axisTextService->timeAxisSource() : nullptr; }
    double effectiveAxisLabelPx() const { return m_axisTextService ? m_axisTextService->effectiveAxisLabelPx() : AxisLayout::kAutoAxisLabelBasePx; }
    int priceAxisWidthPx() const { return m_axisTextService ? m_axisTextService->priceAxisWidthPx() : 90; }
    int timeAxisHeightPx() const { return m_axisTextService ? m_axisTextService->timeAxisHeightPx() : 30; }
    
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
    // The drawn tick (the GPU layer's).
    double heatmapTickSize() const;

    // ── GPU heatmap renderer (S6b, docs/research/2026-10-s6-plan.md) ─────────
    // The process service (MainWindowGPU owns it; it outlives this item). nullptr
    // detaches the layer's controller (teardown).
    void setHeatmapService(heatmap::HeatmapDataService* service);
    bool tradesAboveCandles() const { return m_tradesAboveCandles; }
    // GUI-thread binding only; render callbacks retain the plain C++ state.
    std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const { return m_tradeBubbleFrame; }
    // Chart settings (tick policy, palette, sensitivity, budgets).
    void setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings, bool explicitManualTick = false);
    void setHeatmapTickMemory(const heatmap::ManualTickMemory& memory);
    heatmap::gpu::HeatmapGpuLayer* gpuHeatmapLayer() const { return m_gpuLayer.get(); }
    // Tests: the S7b label layout of the last gpu frame and its glyphs.
    const heatmap::gpu::HeatmapLabelLayout& gpuLabelLayout() const { return m_gpuLabels; }
    const std::vector<ChartGlyphInstance>& gpuLabelGlyphs() const { return m_heatmapLabelGlyphs; }
    uint64_t gpuLabelSerial() const { return m_gpuLabelSerial; } // the LabelCells serial drawn (0: none)

    // ── Auto price scale and fits ──
    // docs/research/2026-10-viewport-autoscale.md. Auto price scale (default on): the price range follows the visible candles' high/low (plus
    // kFitPriceMargin each side) on every viewport change and on candle updates in
    // view, inside the same setViewport (one viewportVersion step, none when the fit
    // is unchanged); no vertical pan; the wheel zooms time. A price zoom turns it off;
    // off, the price range stays where the user left it (also across timeframe
    // switches). A symbol switch keeps the state: on fits the new symbol's candles,
    // off carries the span as a fraction of the price with the current price at the
    // same screen height.
    bool autoPriceScale() const { return m_viewState && m_viewState->autoPriceScale(); }
    // On: the price-axis double-click (fitPriceToData). Off: price stays put.
    void setAutoPriceScale(bool enabled);
    // One viewport change (one viewportVersion step) per call; follow-live is kept.
    // Time: the data's available range (recording availability, else the candle
    // series), at most maxTimeSpanMs; following live (or when it all fits) it ends
    // one padding past the live bucket, else it keeps the view centre inside the data.
    // Price (turns auto price scale on): the visible candles' high/low plus a
    // kFitPriceMargin margin each side, inside the min/max price spans (when the Manual
    // max cuts it, the max span centred on the current price: book mid, last trade,
    // newest close); no visible candle: the span kept, centred on the live price (book
    // mid, last trade, decoded data).
    Q_INVOKABLE bool fitView(bool time, bool price);
    Q_INVOKABLE bool fitTimeToData() { return fitView(true, false); }
    Q_INVOKABLE bool fitPriceToData() { return fitView(false, true); }
    // The default view (time-axis double-click): auto price scale on, follow-live on,
    // the initial span (initial_column_px per bar) ending one padding past the live
    // bucket, price fitted. One viewport change; false while no live anchor is known.
    Q_INVOKABLE bool resetView();
    static constexpr double kFitPriceMargin = 0.06;
    // A pending auto-off symbol carry waits this long after the new symbol's first
    // usable price for its recorded price scale; then (live-only symbol) it is applied
    // from the live price with no Manual max span.
    static constexpr int kCarryLiveOnlyWaitMs = 2000;
    // POST /api/v1/viewport (validated by AgentApiCodec). The flags and the final
    // window are resolved first and committed as ONE viewport change. Explicit price
    // bounds (even equal to the current ones) turn auto price scale and follow-live
    // off; autoScale:true fits (candles, else the live price); fit stands alone.
    // Returns an empty string, or the error code (fit_unavailable, viewport_unavailable).
    struct ViewportRequest {
        std::optional<qint64> startMs, endMs;
        std::optional<double> priceMin, priceMax;
        std::optional<bool> followLive, autoScale;
        QString fit;
    };
    QString applyViewportRequest(const ViewportRequest& request);
    QObject* candleBuffer() const;
    void setCandleBuffer(QObject* buffer);

    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    void setActiveSymbol(const QString& symbol);
    void setLiveBookTop(double bestBid, double bestAsk);
    
    Q_INVOKABLE void setPriceResolution(double resolution);
    Q_INVOKABLE int getCurrentTimeResolution() const;
    Q_INVOKABLE double getCurrentPriceResolution() const;
    Q_INVOKABLE void setGridResolution(int timeResMs, double priceRes);
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
    Q_INVOKABLE double getCacheHitRate() const;

    Q_INVOKABLE QString getMsdfAtlasMemory() const;
    TimeAxisMapping lastTimeAxisMapping() const { return currentTimeAxisMapping(); }
    MappingFrameContext currentFrameContext() const override;
    TimeAxisMapping currentTimeAxisMapping() const override;
    // GUI thread: the raster camera the next frame computes from the current view
    // (same inputs and hysteresis state; a tick change in that frame aside). The axis
    // models, the drawn window posted to the controller and the pan commit use it;
    // hit tests use the published frame (currentFrameContext().raster).
    chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const;
    void applyClientConfig(const ClientConfig& config);
    void applyTpoConfig(const ClientTpoConfig& tpo);
    void applyServerConfig(const ServerConfig& config);

    Q_INVOKABLE void setGridResolutionPreset(int preset);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void zoomAt(double rawDelta, double centerX, double centerY,
                            double viewportWidth = -1.0,
                            double viewportHeight = -1.0);
    Q_INVOKABLE void zoomTimeAt(double rawDelta, double centerX,
                                double viewportWidth = -1.0);
    Q_INVOKABLE void zoomPriceAt(double rawDelta, double centerY,
                                 double viewportHeight = -1.0);
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void beginPanAt(double x, double y);
    Q_INVOKABLE void updatePanAt(double x, double y);
    Q_INVOKABLE void endPanAt();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
    
    Q_INVOKABLE QPointF worldToScreen(qint64 timestamp_ms, double price) const;
    Q_INVOKABLE QPointF screenToWorld(double screenX, double screenY) const;
    Q_INVOKABLE double getScreenWidth() const;
    Q_INVOKABLE double getScreenHeight() const;
    void setHeatmapGamma(double gamma);
    void setHeatmapContrast(double contrast);
    void setHeatmapShaderFloor(double floor);
    void setPrimaryField(int field);
    Q_INVOKABLE void setHeatmapLayerEnabled(bool enabled);
    Q_INVOKABLE void setFootprintLayerEnabled(bool enabled);
    Q_INVOKABLE void setTpoLayerEnabled(bool enabled);

public:
    void onTradeReceived(const Trade& trade);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    void onViewportChanged();

signals:
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
    void candleStyleChanged();
    void candleAppearanceChanged();
    void heatmapGammaChanged();
    void heatmapContrastChanged();
    void heatmapShaderFloorChanged();
    void primaryFieldChanged();
    void layerVisibilityChanged();
    void tpoConfigChanged();
    // TPO layout or theme changed (look only: no history request, unlike tpoConfigChanged).
    void tpoStyleChanged();
    void viewportChanged();
    void timeframeChanged();
    void panVisualOffsetChanged();
    void heatmapTickSizeChanged();
    void tradeBubbleSettingsChanged();
    void axisSourcesChanged();
    void axisLayoutChanged();
    void candleBufferChanged();
    void autoPriceScaleChanged();
    void liveRenderTick();
    // A frame drew new whole pixels per row or column, a new device pixel ratio or
    // surface (queued from the render thread; not per frame).
    void rasterChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void buildMsdfAtlas();
    void connectDataProcessorSignals();
    // The one root: a plain QSGNode whose first child is an opacity node holding
    // the HeatmapTileNode (opacity 0 blocks it while the heatmap layer is off);
    // overlays and text follow it as later children (drawn on top).
    void publishTradeBubbleFrame(const TimeAxisMapping& mapping);
    QSGNode* ensureGpuRootNode(QSGNode* oldNode, heatmap::gpu::HeatmapTileNode** tile);
    QSGNode* updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile);
    void updateGpuLabels(const FrameContext& frame, bool prepared);
    // The frame's raster camera and its TimeAxisMapping (whole-pixel mapping, once
    // per frame, at the layer's tick for this frame).
    void computeGpuFrameMapping(FrameContext& frame);
    void prepareSyncFrame();
    chart_raster::RasterInputs rasterInputs(const FrameViewportSnapshot& viewport, double tick, double width,
                                            double height, double dpr) const;
    // The committed shifts for a drag (GridViewState::PanShift).
    bool rasterPanShift(QPointF drag, qint64& timeShiftMs, double& priceShift) const;
    void renderTradeOverlays(QSGNode* parent, const FrameContext& frame, bool drawFootprint, bool drawTpo,
                             std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads);
    void syncGpuView();
    void applyGpuLimits();
    void followGpuLive();
    void seedGpuViewport(double bestBid, double bestAsk);
    void setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax);
    // "Return to live" (follow-live activation): the view's right edge goes one
    // padding past the live anchor's bucket, in either direction, span kept.
    void returnGpuToLive();
    // The view end that puts the live anchor's bucket one padding inside the right
    // edge for a view of spanMs (0 while no live anchor is known).
    qint64 gpuLiveEndMs(qint64 spanMs) const;
    // Cold start without a book or trade: a time-only view from the recording's
    // availability (price unknown until decoded data supplies it).
    void bootstrapGpuTimeView();
    QTimer* m_gpuBootstrapTimer = nullptr;
    // Auto-fit (fitView). Window pieces return nullopt when nothing is known.
    std::optional<std::pair<qint64, qint64>> gpuFitTimeWindow() const;
    // candlesOnly: no live-price fallback (the auto price scale's fit).
    std::optional<std::pair<double, double>> gpuFitPriceWindow(qint64 start, qint64 end, bool candlesOnly) const;
    // GridViewState's price fit while auto price scale is on. 
    bool autoPriceFit(qint64 start, qint64 end, double& priceMin, double& priceMax);
    // Auto price scale on: the current time range through setViewport (a bump only
    // when the fitted range changed). Candle updates in view and new limits use it.
    void refitAutoPrice();
    double gpuLivePrice() const;
    QPointer<CandleSeriesBuffer> m_candleBuffer;
    QMetaObject::Connection m_candleDirtyConn;
    mutable std::vector<CandleSeriesBuffer::CandleBar> m_fitBars; // gpuFitPriceWindow scratch
    // Symbol switch with auto price scale off: the old span / price and the current
    // price's height in the view (0 = bottom), applied when the new price is known.
    struct PriceCarry { double spanRatio = 0.0, heightFrac = 0.0; };
    std::optional<PriceCarry> m_priceCarry;
    // The carry onto price `now`, one viewport change; kept pending until the layer's
    // price scale is the new symbol's. False (nothing applied) for an unusable price.
    // liveOnly: the live-only fallback (consume without a recorded price scale).
    bool applyPriceCarry(double now, bool liveOnly = false);
    QTimer* m_carryWaitTimer = nullptr;
    double m_gpuBookMid = 0.0;   // newest book-top mid of the active symbol
    double m_gpuLastTrade = 0.0; // newest trade price of the active symbol
    qint64 gpuInitialSpanMs(double widthPx) const;
    void syncGpuSurface();
    void syncGpuTone();
    void bindWindow(QQuickWindow* window);
    void publishFrameContext(const FrameContext& frame);
    void setPriceAxisSource(QObject* source);
    void setTimeAxisSource(QObject* source);

private:
    void setMinVolumeFilter(double minVolume);
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowRenderStrategyOverlay(bool show);
    void setShowViewportMathOverlay(bool show);
    void setShowMemoryCacheOverlay(bool show);
    void setShowModeFlagsOverlay(bool show);
    void setAutoScrollPaddingFrac(double fraction);
    void setAutoScrollSmoothEnabled(bool enabled);

    std::unique_ptr<GridViewState> m_viewState;
    std::unique_ptr<DataProcessor> m_dataProcessor;
    std::unique_ptr<QThread> m_dataProcessorThread;
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_dataProcessor.get(); }
};
