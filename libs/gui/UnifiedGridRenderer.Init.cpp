// UnifiedGridRenderer initialization and data-thread wiring.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"

#include "config/GuiConfigStore.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "chart/shell/HeatmapChartPort.hpp"
void UnifiedGridRenderer::init() {
    const auto& store = GuiConfigStore::instance();
    const auto defaultTimeframeMs = store.hasServerConfig() && store.serverConfig().chart.defaultTimeframeMs > 0
                                        ? store.serverConfig().chart.defaultTimeframeMs : 100;
    const auto& client = store.clientConfig();
    m_heatmapGamma = client.heatmap.gamma;
    m_heatmapContrast = client.heatmap.contrast;
    m_heatmapShaderFloor = client.heatmap.shaderFloor;
    qRegisterMetaType<Trade>("Trade");
    m_frameClock.start();
    m_timeAuthority.setActiveTimeframeMs(defaultTimeframeMs);
    m_gpuLayer = std::make_unique<heatmap::gpu::HeatmapGpuLayer>();
    m_heatmapPort = std::make_unique<chart::HeatmapChartPort>(*this, *m_gpuLayer);
    m_view = std::make_unique<chart::ChartViewController>(*m_heatmapPort, *m_heatmapPort, *m_heatmapPort, m_published, this);
    bindLivePlacement();
    bindTimeframeSwitch();
    m_view->session().timeframeMs = defaultTimeframeMs;
    m_gpuLayer->setTone({static_cast<float>(m_heatmapGamma), static_cast<float>(m_heatmapContrast),
                         static_cast<float>(m_heatmapShaderFloor)});
    bindViewReactions(ReactionPhase::Heatmap);
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::labelsChanged, this, [this] { update(); });
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::buildFailed, this, [](const QString& message) {
        sLog_Warning("GPU heatmap span build failed: " << message);
    });
    buildMsdfAtlas();
    m_axisTextService = std::make_unique<AxisTextService>(m_chartTextAtlas, this);
    connect(m_axisTextService.get(), &AxisTextService::axisSourcesChanged,
            this, &UnifiedGridRenderer::axisSourcesChanged);
    connect(m_axisTextService.get(), &AxisTextService::layoutChanged,
            this, &UnifiedGridRenderer::axisLayoutChanged);
    connect(m_axisTextService.get(), &AxisTextService::needsUpdate,
            this, [this]() { update(); });
    m_axisTextService->refreshAxisLayout();
    m_feeds = std::make_unique<chart::TradeOverlayFeeds>(m_layers, this, [this] { update(); });
    applyClientConfig(store.clientConfig());
    if (store.hasServerConfig()) {
        applyServerConfig(store.serverConfig());
    }
    connect(&store, &GuiConfigStore::clientConfigUpdated, this,
            [this](const ClientConfig& config) { applyClientConfig(config); });
    connect(&store, &GuiConfigStore::serverConfigUpdated, this,
            [this](const ServerConfig& config) { applyServerConfig(config); });
    m_feeds->connectDataProcessorSignals();
    m_feeds->startThread();
    if (width() > 0 && height() > 0) {
        getViewState()->setViewportSize(width(), height());
    }
    connect(getViewState(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::viewportChanged);
    connect(getViewState(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::onViewportChanged);
    connect(getViewState(), &GridViewState::panVisualOffsetChanged, this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(getViewState(), &GridViewState::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    bindViewReactions(ReactionPhase::ViewState);
    // Auto price scale (gpu): every setViewport takes its price from the visible candles.
    getViewState()->setPriceFit([this](qint64 start, qint64 end, double& priceMin, double& priceMax) {
        return m_view->priceFitter().autoPriceFit(start, end, priceMin, priceMax);
    });
    // Whole-pixel mapping: a drag commits exactly the whole device pixels it shows, and
    // a zoom keeps what is drawn under the cursor.
    getViewState()->setPanShift([this](QPointF drag, qint64& timeShiftMs, double& priceShift) {
        return m_view->camera().panShift(drag, timeShiftMs, priceShift);
    });
    connect(getViewState(), &GridViewState::priceInteracted, this, [this]() {
        m_view->priceCarry().priceInteracted();
    });
    
    m_feeds->startProcessing();

    // The GPU heatmap is active from construction: the default view model is the
    // auto price scale; the first view comes from the service's availability, a
    // book top, a trade or the API (bootstrapGpuTimeView, seedGpuViewport).
    m_gpuLayer->setSymbol(m_view->session().symbol.toStdString());
    m_gpuLayer->setTimeframeMs(m_view->session().timeframeMs);
    m_gpuLayer->setActive(true);
    getViewState()->setAutoPriceScale(true);
    m_view->applyGpuLimits();
    m_view->livePlacement().bootstrapGpuTimeView();
}
