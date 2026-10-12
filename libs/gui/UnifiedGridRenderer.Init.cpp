// UnifiedGridRenderer initialization and data-thread wiring.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include <QMetaObject>
#include <QThread>
#include <limits>

#include "render/DataProcessor.hpp"
#include "render/VolumeProfileState.hpp"
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
    m_overlays = { &m_footprintOverlay, &m_tpoOverlay, &m_vpRenderer };
    buildMsdfAtlas();
    m_axisTextService = std::make_unique<AxisTextService>(m_chartTextAtlas, this);
    connect(m_axisTextService.get(), &AxisTextService::axisSourcesChanged,
            this, &UnifiedGridRenderer::axisSourcesChanged);
    connect(m_axisTextService.get(), &AxisTextService::layoutChanged,
            this, &UnifiedGridRenderer::axisLayoutChanged);
    connect(m_axisTextService.get(), &AxisTextService::needsUpdate,
            this, [this]() { update(); });
    m_axisTextService->refreshAxisLayout();
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessorThread->setObjectName(QStringLiteral("DataProcessor"));
    m_dataProcessor = std::make_unique<DataProcessor>();
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    applyClientConfig(store.clientConfig());
    if (store.hasServerConfig()) {
        applyServerConfig(store.serverConfig());
    }
    connect(&store, &GuiConfigStore::clientConfigUpdated, this,
            [this](const ClientConfig& config) { applyClientConfig(config); });
    connect(&store, &GuiConfigStore::serverConfigUpdated, this,
            [this](const ServerConfig& config) { applyServerConfig(config); });
    connectDataProcessorSignals();
    m_dataProcessorThread->start();
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
    
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        &DataProcessor::startProcessing,
        Qt::QueuedConnection);

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

void UnifiedGridRenderer::connectDataProcessorSignals() {
    connect(m_dataProcessor.get(), &DataProcessor::footprintColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray columnQ16, TradeOverlayGrid grid) {
                const bool validGrid = gridWidth > 0 && gridHeight > 0 &&
                    gridHeight <= (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)));
                const int expectedBytes = validGrid ? gridHeight * static_cast<int>(sizeof(uint16_t)) : 0;
                if (!validGrid || columnQ16.size() != expectedBytes || x < 0 || x >= gridWidth) {
                    sLog_RenderN(1000, "footprint column dropped: x=" << x
                                 << " grid=" << gridWidth << "x" << gridHeight
                                 << " bytes=" << columnQ16.size() << " expectedBytes=" << expectedBytes);
                    return;
                }

                m_footprintOverlay.enqueue(
                    FootprintOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(columnQ16), grid});
                sLog_Probe("footprint.queue",
                           "x=" << x << " grid=" << gridWidth << "x" << gridHeight);
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::tpoColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray letters,
                   int64_t sessionStartMs, int64_t sessionEndMs, int64_t timeframeMs, TradeOverlayGrid grid) {
                if (gridWidth <= 0 || gridHeight <= 0 || x < 0 || x >= gridWidth ||
                    letters.size() != gridHeight) {
                    sLog_RenderN(1000, "tpo column dropped: x=" << x
                                 << " grid=" << gridWidth << "x" << gridHeight
                                 << " bytes=" << letters.size());
                    return;
                }
                if (grid.startMs != sessionStartMs || grid.endMs != sessionEndMs) {
                    sLog_RenderN(1000, "tpo column dropped: session mismatch grid=["
                                 << grid.startMs << ".." << grid.endMs << "] session=["
                                 << sessionStartMs << ".." << sessionEndMs << "]");
                    return;
                }
                m_tpoOverlay.enqueue(TpoOverlayRenderer::PendingUpload{
                    x, gridWidth, gridHeight, std::move(letters), grid, timeframeMs});
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::volumeProfileReady,
            this,
            [this](std::vector<float> bins, VolumeProfileState::Snapshot snap) {
                if (bins.empty()) return;
                m_vpRenderer.enqueue(std::move(bins), std::move(snap));
                update();
            },
            Qt::QueuedConnection);
}
