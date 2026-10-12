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
#include "chart/layers/ChartLayers.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "chart/feeds/TradeOverlayFeeds.hpp"
#include "render/FootprintOverlayRenderer.hpp"
#include "chart/feeds/TradeBubbleFeed.hpp"
#include "chart/shell/IChartCameraSource.hpp"
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
class UnifiedGridRenderer : public QQuickItem, public ITimeAxisMappingProvider, public chart::ITradeBubbleFrameSource, public chart::IChartCameraSource {
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
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer WRITE setCandleBuffer NOTIFY candleBufferChanged)
    Q_PROPERTY(bool autoPriceScale READ autoPriceScale WRITE setAutoPriceScale NOTIFY autoPriceScaleChanged)

public:
    void setAgentControlRevision(uint64_t revision, uint64_t selectionEpoch, uint64_t viewportVersion) {
        m_frames.ack.setAgentControlRevision(revision, selectionEpoch, viewportVersion);
        update();
    }
    uint64_t renderedControlRevision() const { return m_frames.ack.renderedControlRevision(); }
    uint64_t renderedFrameId() const { return m_frames.ack.renderedFrameId(); }
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer();
    bool autoScrollEnabled() const { return getViewState() ? getViewState()->isAutoScrollEnabled() : false; }
    int candleStyle() const { return m_look.candleStyle(); }
    void setCandleStyle(int style);
    QColor candleUpColor() const { return m_look.candleUpColor(); }
    QColor candleDownColor() const { return m_look.candleDownColor(); }
    QString candleWickColor() const { return m_look.candleWickColor(); }
    double candleBodyOpacity() const { return m_look.candleBodyOpacity(); }
    int candleWickWidth() const { return m_look.candleWickWidth(); }
    void setCandleAppearance(const heatmap::HeatmapChartSettings& settings);
    double heatmapGamma() const { return m_look.gamma(); }
    double heatmapContrast() const { return m_look.contrast(); }
    double heatmapShaderFloor() const { return m_look.shaderFloor(); }
    int primaryField() const { return m_visibility.primaryField(); }
    bool heatmapLayerEnabled() const { return m_visibility.heatmapEnabled(); }
    bool footprintLayerEnabled() const { return m_visibility.footprintEnabled(); }
    bool tpoLayerEnabled() const { return m_visibility.tpoEnabled(); }
    bool volumeProfileLayerEnabled() const { return m_visibility.volumeProfileEnabled(); }
    int tpoTimeframeMs() const { return m_look.tpoTimeframeMs(); }
    int tpoSessionType() const { return m_look.tpoSessionType(); }
    Q_INVOKABLE void setVolumeProfileLayerEnabled(bool enabled);
    Q_INVOKABLE void setTpoTimeframeMs(int timeframeMs);
    Q_INVOKABLE void setTpoSessionType(int sessionType);
    Q_INVOKABLE void setTpoLayout(const QString& layout);
    Q_INVOKABLE void setTpoTheme(const QString& theme);
    QString tpoLayout() const;
    QString tpoTheme() const;
    int tpoSessions() const { return m_look.tpoSessions(); }
    GridViewState* getViewState() const override { return m_view ? m_view->viewPtr() : nullptr; }
    double chartWidth() const override { return width(); }
    double chartHeight() const override { return height(); }
    QObject* viewState() const { return getViewState(); }
    QObject* priceAxisSource() const { return m_layers.text.axes ? m_layers.text.axes->priceAxisSource() : nullptr; }
    QObject* timeAxisSource() const { return m_layers.text.axes ? m_layers.text.axes->timeAxisSource() : nullptr; }
    double effectiveAxisLabelPx() const { return m_layers.text.axes ? m_layers.text.axes->effectiveAxisLabelPx() : AxisLayout::kAutoAxisLabelBasePx; }
    int priceAxisWidthPx() const { return m_layers.text.axes ? m_layers.text.axes->priceAxisWidthPx() : 90; }
    int timeAxisHeightPx() const { return m_layers.text.axes ? m_layers.text.axes->timeAxisHeightPx() : 30; }
    bool showGpuStatsOverlay() const { return m_debugFlags.gpuStats; }
    bool showDataPipelineOverlay() const { return m_debugFlags.dataPipeline; }
    bool showViewportMathOverlay() const { return m_debugFlags.viewportMath; }
    Q_INVOKABLE qint64 getVisibleTimeStart() const;
    Q_INVOKABLE qint64 getVisibleTimeEnd() const;
    Q_INVOKABLE double getMinPrice() const;
    Q_INVOKABLE double getMaxPrice() const;
    int getCurrentTimeframe() const { return static_cast<int>(m_view->session().timeframeMs); }
    Q_INVOKABLE QPointF getPanVisualOffset() const;
    double heatmapTickSize() const;

    // ── GPU heatmap renderer ──
    // The process service (MainWindowGPU owns it; it outlives this item). nullptr
    // detaches the layer's controller (teardown).
    void setHeatmapService(heatmap::HeatmapDataService* service);
    bool tradesAboveCandles() const { return m_bubbles.tradesAboveCandles(); }
    // GUI-thread binding only; render callbacks retain the plain C++ state.
    std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const override { return m_bubbles.tradeBubbleRenderFrame(); }
    void setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings, bool explicitManualTick = false);
    void setHeatmapTickMemory(const heatmap::ManualTickMemory& memory);
    heatmap::gpu::HeatmapGpuLayer* gpuHeatmapLayer() const { return m_layers.heatmap.gpu.get(); }

    // ── Auto price scale and fits ──
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
    chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const override;
    void applyClientConfig(const ClientConfig& config);
    void applyTpoConfig(const ClientTpoConfig& tpo);
    void applyServerConfig(const ServerConfig& config);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    // A click (mouse wheel
    // notch, keyboard, toolbar) moves to the next rung, a view with whole device pixels
    // per column and per row, by a kZoomGlideMs eased glide drawn with coverage; a
    // pinch or trackpad scroll zooms continuously and settles on the nearest rung.
    // Item coordinates (logical px); time/price select the axes.
    Q_INVOKABLE void zoomClicks(int clicks, double x, double y, bool time = true, bool price = true);
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
    bool zoomGesturing() const { Q_ASSERT(m_view); return m_view->glide().gesture; }
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
    void onTradeReceived(const Trade& trade);
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    void onViewportChanged();

    // Test hooks (forwarders only)
    const heatmap::gpu::HeatmapLabelLayout& gpuLabelLayout() const { return m_layers.heatmap.labelLayout(); }
    const std::vector<ChartGlyphInstance>& gpuLabelGlyphs() const { return m_layers.heatmap.labelGlyphs(); }
    uint64_t gpuLabelSerial() const { return m_layers.heatmap.labelSerial(); }
    int glideRebasesForTest() const { return m_view->glide().rebases; }
    void followLiveForTest(qint64 openEndMs) { m_view->livePlacement().followGpuLiveTo(openEndMs); }
    bool gestureSettlePending() const { return m_view->zoom().gestureSettlePending(); }
    void setZoomClockForTest(std::function<qint64()> clock) { m_clock.zoom = std::move(clock); }
    // End test hooks

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
    qint64 zoomNowMs() const;
    void bindZoomGlide();
    void bindLivePlacement();
    void bindTimeframeSwitch();
    enum class ReactionPhase { Heatmap, ViewState, Ready };
    void bindViewReactions(ReactionPhase phase);
    // The one root: a plain QSGNode whose first child is an opacity node holding
    // the HeatmapTileNode (opacity 0 blocks it while the heatmap layer is off);
    // overlays and text follow it as later children (drawn on top).
    QSGNode* updateGpuPaintNode(QSGNode* oldNode, FrameContext& frame, bool profile);
    void prepareSyncFrame();
    void refitAutoPrice();
    void syncGpuSurface();
    void bindWindow(QQuickWindow* window);
    void setPriceAxisSource(QObject* source);
    void setTimeAxisSource(QObject* source);

private:
    void setShowGpuStatsOverlay(bool show);
    void setShowDataPipelineOverlay(bool show);
    void setShowViewportMathOverlay(bool show);
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_feeds ? m_feeds->processor() : nullptr; }

private:
    struct DebugFlags { bool gpuStats = false, dataPipeline = false, viewportMath = false; } m_debugFlags;
    FrameProfiler m_frameProfiler;
    TimeAuthority m_timeAuthority;
    struct Connections {
        QMetaObject::Connection screenChanged, candleDirty;
        struct { QMetaObject::Connection beforeSync, afterRender; } render;
    } m_connections;
    struct ClockState {
        QElapsedTimer frame;
        std::function<qint64()> zoom;
        QTimer* glidePolish = nullptr;
    } m_clock;
    chart::TradeBubbleFeed m_bubbles;
    chart::ChartLayers m_layers;
    chart::LayerVisibility m_visibility;
    chart::ChartLookSettings m_look{m_layers};
    struct Frames {
        chart::PublishedFrame published;
        chart::FrameProducer producer;
        chart::FrameAck ack;
    } m_frames;
    chart::CandleBufferSource m_candleSource;
    std::unique_ptr<chart::HeatmapChartPort> m_heatmapPort;
    std::unique_ptr<chart::ChartViewController> m_view;
    std::unique_ptr<chart::TradeOverlayFeeds> m_feeds;
};
