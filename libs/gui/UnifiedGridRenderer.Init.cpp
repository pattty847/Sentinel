// UnifiedGridRenderer init/data-thread wiring split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
#include <QDateTime>
#include <QMetaObject>
#include <QMetaType>
#include <QElapsedTimer>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <limits>

#include "render/DataProcessor.hpp"
#include "render/VolumeProfileState.hpp"
#include "config/GuiConfigStore.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
void UnifiedGridRenderer::init() {
    const auto& store = GuiConfigStore::instance();
    if (store.hasServerConfig() && store.serverConfig().heatmap.activeTimeframeMs > 0)
        m_currentTimeframe_ms = store.serverConfig().heatmap.activeTimeframeMs;
    const auto& client = store.clientConfig();
    m_heatmapGamma = client.heatmap.gamma;
    m_heatmapContrast = client.heatmap.contrast;
    m_heatmapShaderFloor = client.heatmap.shaderFloor;
    qRegisterMetaType<Trade>("Trade");
    m_frameClock.start();
    m_timeAuthority.setActiveTimeframeMs(m_currentTimeframe_ms);

    m_viewState = std::make_unique<GridViewState>(this);
    // S6b: the per-chart GPU heatmap layer (the only heatmap renderer since S8a).
    m_gpuLayer = std::make_unique<heatmap::gpu::HeatmapGpuLayer>();
    m_gpuLayer->setTone({static_cast<float>(m_heatmapGamma), static_cast<float>(m_heatmapContrast),
                         static_cast<float>(m_heatmapShaderFloor)});
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::snapshotChanged, this, [this] {
        // No book top yet (or a symbol switch): centre on the decoded data instead.
        if ((!m_gpuPriceKnown || m_gpuReseedPrice) && m_viewState->isTimeWindowValid()) {
            if (const double mid = m_gpuLayer->recentMidPrice(); mid > 0) seedGpuViewport(mid, mid);
        }
        update();
    });
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::liveChanged, this, [this] {
        followGpuLive();
        emit liveRenderTick();
        update();
    });
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::tickChanged, this, [this] {
        applyGpuLimits();
        refitAutoPrice(); // the fit's min/max price spans follow the tick
        syncGpuView();    // the drawn rows (and so the drawn price window) follow it too
        emit heatmapTickSizeChanged();
        update();
    });
    connect(m_gpuLayer.get(), &heatmap::gpu::HeatmapGpuLayer::limitsChanged, this, [this] {
        applyGpuLimits();
        // A carry applied before the new symbol's price scale was known: again, now
        // that its limits are.
        if (m_priceCarry && !m_gpuReseedPrice && !m_gpuLimitsDeferred &&
            m_gpuLayer->priceScaleCurrent())
            applyPriceCarry(gpuLivePrice());
        refitAutoPrice(); // e.g. a new symbol's tick: its min/max price spans
    });
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
        m_viewState->setViewportSize(width(), height());
    }
    
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::viewportChanged);
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::onViewportChanged);
    connect(m_viewState.get(), &GridViewState::panVisualOffsetChanged, this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(m_viewState.get(), &GridViewState::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    connect(m_viewState.get(), &GridViewState::autoPriceScaleChanged, this, [this]() {
        sLog_Render("auto price scale=" << m_viewState->autoPriceScale());
        if (m_viewState->autoPriceScale()) m_priceCarry.reset(); // on: the candles fit
        emit autoPriceScaleChanged();
    });
    // Auto price scale (gpu): every setViewport takes its price from the visible candles.
    m_viewState->setPriceFit([this](qint64 start, qint64 end, double& priceMin, double& priceMax) {
        return autoPriceFit(start, end, priceMin, priceMax);
    });
    // Whole-pixel mapping: a drag commits exactly the whole device pixels it shows.
    m_viewState->setPanShift([this](QPointF drag, qint64& timeShiftMs, double& priceShift) {
        return rasterPanShift(drag, timeShiftMs, priceShift);
    });
    connect(m_viewState.get(), &GridViewState::priceInteracted, this, [this]() {
        m_priceCarry.reset(); // the user owns price now: no pending carry replaces it
    });
    
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        &DataProcessor::startProcessing,
        Qt::QueuedConnection);

    // The GPU heatmap is active from construction: the default view model is the
    // auto price scale; the first view comes from the service's availability, a
    // book top, a trade or the API (bootstrapGpuTimeView, seedGpuViewport).
    m_gpuLayer->setSymbol(m_activeSymbol.toStdString());
    m_gpuLayer->setTimeframeMs(m_currentTimeframe_ms);
    m_gpuLayer->setActive(true);
    m_viewState->setAutoPriceScale(true);
    applyGpuLimits();
    bootstrapGpuTimeView();
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
                m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
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
