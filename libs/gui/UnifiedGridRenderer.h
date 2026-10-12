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
#include "chart/shell/ChartViewQml.hpp"
#include "chart/shell/ChartDebugQml.hpp"
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
class ChartAgentPort;
class UnifiedGridRenderer : public QQuickItem, public ITimeAxisMappingProvider, public chart::ITradeBubbleFrameSource, public chart::IChartCameraSource {
    friend struct TradeBubbleRendererTest;
    friend class chart::HeatmapChartPort;
    Q_OBJECT
    Q_INTERFACES(ITimeAxisMappingProvider)
    QML_ELEMENT
    Q_PROPERTY(QObject* priceAxisSource READ priceAxisSource WRITE setPriceAxisSource NOTIFY axisSourcesChanged)
    Q_PROPERTY(QObject* timeAxisSource READ timeAxisSource WRITE setTimeAxisSource NOTIFY axisSourcesChanged)
    Q_PROPERTY(double effectiveAxisLabelPx READ effectiveAxisLabelPx NOTIFY axisLayoutChanged)
    Q_PROPERTY(int priceAxisWidthPx READ priceAxisWidthPx NOTIFY axisLayoutChanged)
    Q_PROPERTY(int timeAxisHeightPx READ timeAxisHeightPx NOTIFY axisLayoutChanged)
    Q_PROPERTY(QObject* candleBuffer READ candleBuffer WRITE setCandleBuffer NOTIFY candleBufferChanged)

    Q_PROPERTY(QObject* view READ viewObject CONSTANT)
    Q_PROPERTY(QObject* candles READ candlesObject CONSTANT)
    Q_PROPERTY(QObject* tpo READ tpoObject CONSTANT)
    Q_PROPERTY(QObject* layers READ layersObject CONSTANT)
    Q_PROPERTY(QObject* debug READ debugObject CONSTANT)

public:
    QObject* viewObject() { return view(); }
    chart::ChartViewQml* view() { return &m_qml.view; }
    const chart::ChartViewQml* view() const { return &m_qml.view; }
    QObject* candlesObject() { return candles(); }
    chart::ChartLookSettings* candles() { return &m_look; }
    const chart::ChartLookSettings* candles() const { return &m_look; }
    QObject* tpoObject() { return tpo(); }
    chart::ChartTpoQml* tpo() { return &m_look.tpo; }
    const chart::ChartTpoQml* tpo() const { return &m_look.tpo; }
    QObject* layersObject() { return layers(); }
    chart::LayerVisibility* layers() { return &m_visibility; }
    const chart::LayerVisibility* layers() const { return &m_visibility; }
    QObject* debugObject() { return debug(); }
    chart::ChartDebugQml* debug() { return &m_qml.debug; }
    const chart::ChartDebugQml* debug() const { return &m_qml.debug; }
    ChartAgentPort agentPort();
    explicit UnifiedGridRenderer(QQuickItem* parent = nullptr);
    ~UnifiedGridRenderer();
    GridViewState* getViewState() const override { return m_view ? m_view->viewPtr() : nullptr; }
    double chartWidth() const override { return width(); }
    double chartHeight() const override { return height(); }
    void connectCameraChanged(QObject* context, std::function<void()> onChange) override;
    QObject* priceAxisSource() const { return m_layers.text.axes ? m_layers.text.axes->priceAxisSource() : nullptr; }
    QObject* timeAxisSource() const { return m_layers.text.axes ? m_layers.text.axes->timeAxisSource() : nullptr; }
    double effectiveAxisLabelPx() const { return m_layers.text.axes ? m_layers.text.axes->effectiveAxisLabelPx() : AxisLayout::kAutoAxisLabelBasePx; }
    int priceAxisWidthPx() const { return m_layers.text.axes ? m_layers.text.axes->priceAxisWidthPx() : 90; }
    int timeAxisHeightPx() const { return m_layers.text.axes ? m_layers.text.axes->timeAxisHeightPx() : 30; }

    // ── GPU heatmap renderer ──
    // The process service (MainWindowGPU owns it; it outlives this item). nullptr
    // detaches the layer's controller (teardown).
    void setHeatmapService(heatmap::HeatmapDataService* service);
    // GUI-thread binding only; render callbacks retain the plain C++ state.
    std::shared_ptr<trade_bubbles::RenderFrame> tradeBubbleRenderFrame() const override { return m_bubbles.tradeBubbleRenderFrame(); }
    void setHeatmapChartSettings(const heatmap::HeatmapChartSettings& settings, bool explicitManualTick = false);
    void setHeatmapTickMemory(const heatmap::ManualTickMemory& memory);
    heatmap::gpu::HeatmapGpuLayer* gpuHeatmapLayer() const { return m_layers.heatmap.gpu.get(); }

    // ── Auto price scale and fits ──
    static constexpr double kFitPriceMargin = chart::PriceFitter::kFitPriceMargin;
    QObject* candleBuffer() const;
    void setCandleBuffer(QObject* buffer);
    void addTrade(const Trade& trade);
    void clearData();
    void setActiveSymbol(const QString& symbol);
    void setLiveBookTop(double bestBid, double bestAsk);
    QObject* viewNotifications() override { return view(); }
    MappingFrameContext currentFrameContext() const override;
    TimeAxisMapping currentTimeAxisMapping() const override;
    chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const override;
    void applyClientConfig(const ClientConfig& config);
    void applyServerConfig(const ServerConfig& config);
    bool zoomGliding() const { Q_ASSERT(m_view); return m_view->glide().time.active || m_view->glide().price.active; }
    bool timeGliding() const { Q_ASSERT(m_view); return m_view->glide().time.active; }
    bool priceGliding() const { Q_ASSERT(m_view); return m_view->glide().price.active; }
    bool zoomGesturing() const { Q_ASSERT(m_view); return m_view->glide().gesture; }
    // The camera the next frame draws without a zoom transition (the rest camera of
    // the stored view; a glide's target).
    chart_raster::RasterCamera restCameraNow(bool includeDrag = true) const;
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
    // TPO layout or theme changed (look only: no history request, unlike tpoConfigChanged).
    void axisSourcesChanged();
    void axisLayoutChanged();
    void candleBufferChanged();
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
    void init();

public:
    DataProcessor* getDataProcessor() const { return m_feeds ? m_feeds->processor() : nullptr; }

private:
    struct QmlObjects { chart::ChartViewQml view; chart::ChartDebugQml debug; } m_qml;
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
