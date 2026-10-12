#pragma once
#include "chart/shell/CandleBufferSource.hpp"
#include <QQuickItem>
#include <QTimer>
#include <QThread>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QColor>
#include <QPointer>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <atomic>
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
#include "chart/core/FrameProducer.hpp"
#include "chart/core/FrameAck.hpp"
#include "render/FrameContext.hpp"
#include "render/TimeAuthority.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/core/ChartViewController.hpp"
// ── Overlay renderers (owned inline) ─────────────────────────────────────────
#include "chart/layers/TradeLayers.hpp"
#include "chart/feeds/TradeOverlayFeeds.hpp"
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
namespace chart { class HeatmapChartPort; }

class UnifiedGridRenderer : public QQuickItem, public ITimeAxisMappingProvider {
    friend struct TradeBubbleRendererTest;
    friend class chart::HeatmapChartPort;
    Q_OBJECT
    Q_INTERFACES(ITimeAxisMappingProvider)
    QML_ELEMENT
    
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    
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
    Q_PROPERTY(bool showViewportMathOverlay READ showViewportMathOverlay WRITE setShowViewportMathOverlay NOTIFY showViewportMathOverlayChanged)

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
    bool m_showGpuStatsOverlay = false;
    bool m_showDataPipelineOverlay = false;
    bool m_showViewportMathOverlay = false;
    int m_candleStyle = 0;
    QColor m_candleUpColor{"#2EBD85"}, m_candleDownColor{"#F6465D"};
    QString m_candleWickColor = "auto"; // each wick follows its body
    double m_candleBodyOpacity = 1;
    int m_candleWickWidth = 1;

    FrameProfiler m_frameProfiler;  // SENTINEL_FRAME_PROFILE=1, render thread only
    // Per-chart GPU heatmap state, read in updatePaintNode while the GUI is blocked.
    std::unique_ptr<heatmap::gpu::HeatmapGpuLayer> m_gpuLayer;
    // The chart's frame clock and timeframe cadence (FrameContext::time; the candle
    // overlay's boundary sequence reads it through currentFrameContext()).
    QElapsedTimer m_frameClock;
    TimeAuthority m_timeAuthority;
    double m_heatmapGamma = 1.05;
    double m_heatmapContrast = 1.15;
    double m_heatmapShaderFloor = 0.01;
    int m_primaryField = 0;
    bool m_heatmapLayerEnabled = true;
    bool m_footprintLayerEnabled = false;
    bool m_tpoLayerEnabled = false;
    bool m_volumeProfileLayerEnabled = false;

    chart::PublishedFrame m_published;
    QMetaObject::Connection m_screenChangedConn;
    std::function<qint64()> m_zoomClock;
    qint64 zoomNowMs() const;
    void bindZoomGlide();
    void bindLivePlacement();
    void bindTimeframeSwitch();
    enum class ReactionPhase { Heatmap, ViewState, Ready };
    void bindViewReactions(ReactionPhase phase);
    QTimer* m_glidePolishTimer = nullptr;
    QMetaObject::Connection m_beforeSyncConn;
    // Render thread: this sync's frame (context, tick, raster camera), prepared in
    // beforeSynchronizing and drawn by updatePaintNode in the same sync.
    chart::FrameProducer m_frameProducer;

    ChartTextAtlas m_chartTextAtlas;
    bool m_chartTextAtlasBuilt = false;
    ChartTextRenderer m_chartTextRenderer;
    std::vector<ChartGlyphInstance> m_heatmapLabelGlyphs;
    heatmap::gpu::HeatmapLabelLayout m_gpuLabels; // render thread (updatePaintNode)
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
    chart::TradeLayers m_layers;
    int m_tpoTimeframeMs = 1800000;           // 30m brackets (config tpo.period_minutes)
    int m_tpoSessionType = 4;                 // SessionManager::SessionType::H24

    chart::FrameAck m_frameAck;

public:
    void setAgentControlRevision(uint64_t revision, uint64_t selectionEpoch, uint64_t viewportVersion) {
        m_frameAck.setAgentControlRevision(revision, selectionEpoch, viewportVersion);
        update();
    }
    uint64_t renderedControlRevision() const { return m_frameAck.renderedControlRevision(); }
    uint64_t renderedFrameId() const { return m_frameAck.renderedFrameId(); }
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer();
    
    bool autoScrollEnabled() const { return getViewState() ? getViewState()->isAutoScrollEnabled() : false; }
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
    int tpoSessions() const { return m_layers.tpo.renderer.style().maxSessions; }
    
    GridViewState* getViewState() const { return m_view ? m_view->viewPtr() : nullptr; }
    QObject* viewState() const { return getViewState(); }
    QObject* priceAxisSource() const { return m_axisTextService ? m_axisTextService->priceAxisSource() : nullptr; }
    QObject* timeAxisSource() const { return m_axisTextService ? m_axisTextService->timeAxisSource() : nullptr; }
    double effectiveAxisLabelPx() const { return m_axisTextService ? m_axisTextService->effectiveAxisLabelPx() : AxisLayout::kAutoAxisLabelBasePx; }
    int priceAxisWidthPx() const { return m_axisTextService ? m_axisTextService->priceAxisWidthPx() : 90; }
    int timeAxisHeightPx() const { return m_axisTextService ? m_axisTextService->timeAxisHeightPx() : 30; }
    
    bool showGpuStatsOverlay() const { return m_showGpuStatsOverlay; }
    bool showDataPipelineOverlay() const { return m_showDataPipelineOverlay; }
    bool showViewportMathOverlay() const { return m_showViewportMathOverlay; }

    Q_INVOKABLE qint64 getVisibleTimeStart() const;
    Q_INVOKABLE qint64 getVisibleTimeEnd() const; 
    Q_INVOKABLE double getMinPrice() const;
    Q_INVOKABLE double getMaxPrice() const;
    
    int getCurrentTimeframe() const { return static_cast<int>(m_view->session().timeframeMs); }
    
    Q_INVOKABLE QPointF getPanVisualOffset() const;
    // The drawn tick (the GPU layer's).
    double heatmapTickSize() const;

    // ── GPU heatmap renderer ──
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
    // Tests: the label layout of the last GPU frame and its glyphs.
    const heatmap::gpu::HeatmapLabelLayout& gpuLabelLayout() const { return m_gpuLabels; }
    const std::vector<ChartGlyphInstance>& gpuLabelGlyphs() const { return m_heatmapLabelGlyphs; }
    uint64_t gpuLabelSerial() const { return m_gpuLabelSerial; } // the LabelCells serial drawn (0: none)

    // ── Auto price scale and fits ──
    // View and fit behavior: docs/ARCHITECTURE.md, "Coordinate system: TimeAxisMapping".
    bool autoPriceScale() const { return getViewState() && getViewState()->autoPriceScale(); }
    void setAutoPriceScale(bool enabled);
    Q_INVOKABLE bool fitView(bool time, bool price);
    Q_INVOKABLE bool fitTimeToData() { return fitView(true, false); }
    Q_INVOKABLE bool fitPriceToData() { return fitView(false, true); }
    Q_INVOKABLE bool resetView();
    static constexpr double kFitPriceMargin = chart::PriceFitter::kFitPriceMargin;
    using ViewportRequest = chart::ViewportRequest;
    QString applyViewportRequest(const ViewportRequest& request);
    QObject* candleBuffer() const;
    void setCandleBuffer(QObject* buffer);

    Q_INVOKABLE void addTrade(const Trade& trade);
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void clearData();
    void setActiveSymbol(const QString& symbol);
    void setLiveBookTop(double bestBid, double bestAsk);
    
    Q_INVOKABLE QString getViewportMathDebug() const;
    Q_INVOKABLE QString getDataPipelineDebug() const;
    
    Q_INVOKABLE QString getPerformanceStats() const;

    Q_INVOKABLE QString getMsdfAtlasMemory() const;
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

    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    
    
    // Whole-pixel smooth zoom (slice A2, docs/ARCHITECTURE.md). A click (mouse wheel
    // notch, keyboard, toolbar) moves to the next rung, a view with whole device pixels
    // per column and per row, by a kZoomGlideMs eased glide drawn with coverage; a
    // pinch or trackpad scroll zooms continuously and settles on the nearest rung.
    // Item coordinates (logical px); time/price select the axes.
    Q_INVOKABLE void zoomClicks(int clicks, double x, double y, bool time = true, bool price = true);
    // factor > 1 zooms in; the gesture settles on a rung at endZoomGesture() or after a pause.
    Q_INVOKABLE void zoomContinuous(double factor, double x, double y, bool time = true, bool price = true);
    Q_INVOKABLE void endZoomGesture();
    // The axes (QML). Unphased mouse notches are clicks (pixel deltas may be present); a
    // trackpad scroll is continuous until its ScrollEnd. Drags are continuous until the
    // release (endZoomGesture). A price zoom takes price over (auto price scale off).
    Q_INVOKABLE void zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x);
    Q_INVOKABLE void zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y);
    Q_INVOKABLE void zoomTimeDrag(double delta, double x);
    Q_INVOKABLE void zoomPriceDrag(double delta, double y);
    Q_INVOKABLE void zoomTimeClicks(int clicks, double x);
    Q_INVOKABLE void zoomPriceClicks(int clicks, double y);
    bool zoomGliding() const { Q_ASSERT(m_view); return m_view->glide().time.active || m_view->glide().price.active; }
    bool timeGliding() const { Q_ASSERT(m_view); return m_view->glide().time.active; }
    bool priceGliding() const { Q_ASSERT(m_view); return m_view->glide().price.active; }
    int glideRebasesForTest() const { return m_view->glide().rebases; }
    // Tests: a follow-live update for a live open end (the fixtures have no live feed).
    void followLiveForTest(qint64 openEndMs) { m_view->livePlacement().followGpuLiveTo(openEndMs); }
    bool zoomGesturing() const { Q_ASSERT(m_view); return m_view->glide().gesture; }
    // Tests: inspect the actual settle timer without depending on its duration.
    bool gestureSettlePending() const { return m_view->zoom().gestureSettlePending(); }
    // Tests: the glide clock (ms); default the chart's frame clock.
    void setZoomClockForTest(std::function<qint64()> clock) { m_zoomClock = std::move(clock); }
    // The camera the next frame draws without a zoom transition (the rest camera of
    // the stored view; a glide's target).
    chart_raster::RasterCamera restCameraNow(bool includeDrag = true) const;
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void beginPanAt(double x, double y);
    Q_INVOKABLE void updatePanAt(double x, double y);
    Q_INVOKABLE void endPanAt();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
    
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
    void autoScrollEnabledChanged();
    void showGpuStatsOverlayChanged();
    void showDataPipelineOverlayChanged();
    void showViewportMathOverlayChanged();
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
    // A frame drew new whole pixels per row or column, a new tick, device pixel ratio
    // or surface (queued from the render thread with viewportChanged; not per frame).
    void rasterChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override; // the trackpad pinch (native zoom gesture)
    void updatePolish() override;        // a zoom glide advances once per frame (GUI thread)

private:
    void buildMsdfAtlas();
    void publishTradeBubbleFrame(const TimeAxisMapping& mapping);
    // The one root: a plain QSGNode whose first child is an opacity node holding
    // the HeatmapTileNode (opacity 0 blocks it while the heatmap layer is off);
    // overlays and text follow it as later children (drawn on top).
    QSGNode* ensureGpuRootNode(QSGNode* oldNode, heatmap::gpu::HeatmapTileNode** tile);
    QSGNode* updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile);
    void updateGpuLabels(const FrameContext& frame, bool prepared);
    // The frame's raster camera and its TimeAxisMapping (whole-pixel mapping, once
    // per frame, at the layer's tick for this frame).
    void prepareSyncFrame();
    void refitAutoPrice();
    chart::CandleBufferSource m_candleSource;
    QMetaObject::Connection m_candleDirtyConn;
    void syncGpuSurface();
    void syncGpuTone();
    void bindWindow(QQuickWindow* window);
    void setPriceAxisSource(QObject* source);
    void setTimeAxisSource(QObject* source);

private:
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowViewportMathOverlay(bool show);

    std::unique_ptr<chart::HeatmapChartPort> m_heatmapPort;
    std::unique_ptr<chart::ChartViewController> m_view;
    std::unique_ptr<chart::TradeOverlayFeeds> m_feeds;
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_feeds ? m_feeds->processor() : nullptr; }
};
