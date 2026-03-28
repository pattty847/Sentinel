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
#include "render/ViewportAutoScrollController.hpp"
#include "render/VolumeProfileState.hpp"
#include "config/GuiConfigStore.hpp"
void UnifiedGridRenderer::init() {
    m_useGpuHeatmap = true;

    const auto& store = GuiConfigStore::instance();
    int heatmapGridWidth = 5120;
    int heatmapGridHeight = 2048;
    if (store.hasServerConfig()) {
        const auto& server = store.serverConfig();
        if (server.heatmap.gridWidth > 0)  heatmapGridWidth  = server.heatmap.gridWidth;
        if (server.heatmap.gridHeight > 0) heatmapGridHeight = server.heatmap.gridHeight;
        if (server.heatmap.activeTimeframeMs > 0) {
            m_currentTimeframe_ms = server.heatmap.activeTimeframeMs;
        }
    }
    const auto& client = store.clientConfig();
    m_heatmapGamma = client.heatmap.gamma;
    m_heatmapContrast = client.heatmap.contrast;
    m_heatmapShaderFloor = client.heatmap.shaderFloor;
    if (client.heatmap.labelPx > 0 && client.heatmap.labelPx <= 128) {
        m_heatmapLabelPx = client.heatmap.labelPx;
    }
    qRegisterMetaType<Trade>("Trade");

    m_viewState = std::make_unique<GridViewState>(this);
    m_heatmapStreamService = std::make_unique<HeatmapStreamService>(this);
    m_heatmapStreamService->init(heatmapGridWidth, heatmapGridHeight,
                                 m_currentTimeframe_ms, 1 /*intensityBytesPerCell*/);
    m_heatmapStreamService->setAutoScrollPaddingFrac(m_autoScrollPaddingFrac);
    m_heatmapStreamService->setAutoScrollSmoothEnabled(m_smoothAutoScrollEnabled);
    m_heatmapOverlay.setGridDimensions(heatmapGridWidth, heatmapGridHeight);
    m_heatmapOverlay.setIntensityBytesPerCell(1);
    m_heatmapOverlay.setBackgroundColor(m_heatmapBackgroundColor);
    m_overlays = { &m_heatmapOverlay, &m_footprintOverlay, &m_tpoOverlay, &m_vpRenderer };
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
    
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        &DataProcessor::startProcessing,
        Qt::QueuedConnection);

    if (m_useGpuHeatmap && m_dataProcessor) {
        const int gw = m_heatmapStreamService->gridWidth();
        const int gh = m_heatmapStreamService->gridHeight();
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this, gw, gh]() {
            m_dataProcessor->setHeatmapGridDimensions(gw, gh);
            m_dataProcessor->setHeatmapIntensityScale(m_intensityScale);
        }, Qt::QueuedConnection);
        startHeatmapRenderLoop();
    }
}

// Helper: build a HeatmapColumnEvent from signal args and delegate to the service.
// Returns true if accepted.
bool UnifiedGridRenderer::ingestHeatmapColumn(
        const HeatmapStreamService::HeatmapColumnEvent& event) {
    m_lastIncomingHeatmapSliceTimeframeMs.store(event.timeframeMs, std::memory_order_relaxed);
    if (!m_useGpuHeatmap) {
        m_useGpuHeatmap = true;
        m_heatmapOverlay.requestFullTextureRebuild();
        m_heatmapStreamService->ensureClockStarted();
    }
    auto result = m_heatmapStreamService->ingestColumn(
        event, m_viewState.get(), m_heatmapOverlay,
        m_liquidityLabelMode, m_currentTimeframe_ms);
    if (!result.accepted) return false;

    if (result.tickSizeChanged) emit heatmapTickSizeChanged();
    if (result.maxLiquidityChanged) emit heatmapMaxObservedLiquidityChanged();
    if (result.minLiquidityChanged) emit heatmapMinObservedLiquidityChanged();
    if (result.autoScrollApplied && m_panSyncPending) {
        m_viewState->clearPanVisualOffset();
        m_panSyncPending = false;
    }
    return true;
}

void UnifiedGridRenderer::connectDataProcessorSignals() {
    using Event = HeatmapStreamService::HeatmapColumnEvent;

    connect(m_dataProcessor.get(), &DataProcessor::heatmapColumnReady,
            this,
            [this](int64_t sliceStartMs, int64_t sliceEndMs, int64_t timeframeMs,
                   double minPrice, double maxPrice, double tickSize,
                   const QByteArray& column, const QByteArray& liquidityColumn,
                   double liquidityScale, int intensityBytesPerCell) {
                Event event;
                event.sliceStartMs = sliceStartMs;
                event.sliceEndMs = sliceEndMs;
                event.timeframeMs = timeframeMs;
                event.minPrice = minPrice;
                event.maxPrice = maxPrice;
                event.tickSize = tickSize;
                event.column = column;
                event.liquidityColumn = liquidityColumn;
                event.liquidityScale = liquidityScale;
                event.intensityBytesPerCell = intensityBytesPerCell;
                if (ingestHeatmapColumn(event)) update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::heatmapHistoryBatchReady,
            this,
            [this](int64_t timeframeMs, int gridWidth, int gridHeight,
                   const QVector<IGridDataSource::HeatmapHistoryColumn>& columns,
                   int intensityBytesPerCell) {
                Q_UNUSED(gridWidth);
                Q_UNUSED(gridHeight);
                bool anyApplied = false;
                for (const auto& col : columns) {
                    Event event;
                    event.sliceStartMs = col.bucketStartMs;
                    event.sliceEndMs = col.bucketEndMs;
                    event.timeframeMs = timeframeMs;
                    event.minPrice = col.minPrice;
                    event.maxPrice = col.maxPrice;
                    event.tickSize = col.tickSize;
                    event.column = col.intensity;
                    event.liquidityColumn = col.liquidity;
                    event.liquidityScale = col.liquidityScale;
                    event.intensityBytesPerCell = intensityBytesPerCell;
                    anyApplied = ingestHeatmapColumn(event) || anyApplied;
                }
                if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                    sLog_Debug(QString("Heatmap history batch ingested: columns=%1")
                                   .arg(columns.size()));
                }
                if (anyApplied) update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::heatmapRangeReset,
            this,
            [this](double minPrice, double maxPrice, double tickSize, int gridWidth, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapOverlay.requestFullTextureRebuild();
                    m_heatmapStreamService->ensureClockStarted();
                }
                auto result = m_heatmapStreamService->handleRangeReset(
                    minPrice, maxPrice, tickSize, gridWidth, gridHeight,
                    m_viewState.get(), m_heatmapOverlay);
                if (result.tickSizeChanged) emit heatmapTickSizeChanged();
                if (m_axisTextService) {
                    if (m_axisTextService->timeAxisModel()) m_axisTextService->timeAxisModel()->recalculateTicks();
                    if (m_axisTextService->priceAxisModel()) m_axisTextService->priceAxisModel()->recalculateTicks();
                }
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::footprintColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray columnQ16) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapStreamService->ensureClockStarted();
                }
                if (gridWidth <= 0 || gridHeight <= 0) return;
                if (gridHeight > (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)))) return;
                const int expectedBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
                if (columnQ16.size() != expectedBytes) return;
                if (x < 0 || x >= gridWidth) return;

                m_footprintOverlay.enqueue(
                    FootprintOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(columnQ16)});
                m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
                if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                    sLog_Debug(QString("Footprint queued for render: x=%1 grid=%2x%3")
                                   .arg(x).arg(gridWidth).arg(gridHeight));
                }
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::tpoColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray letters,
                   int64_t sessionStartMs, int64_t sessionEndMs, int64_t timeframeMs) {
                if (gridWidth <= 0 || gridHeight <= 0 || x < 0 || x >= gridWidth) return;
                if (letters.size() != gridHeight) return;
                m_tpoOverlay.enqueue(
                    TpoOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(letters)},
                    sessionStartMs, sessionEndMs, timeframeMs, gridWidth);
                update();
            },
            Qt::QueuedConnection);

    // ── TPO POC/VAH/VAL ────────────────────────────────────────────────────
    connect(m_dataProcessor.get(), &DataProcessor::tpoPocVahValReady,
            this,
            [this](int pocRow, int vahRow, int valRow,
                   int gridHeight, double maxPrice, double tickSize) {
                std::lock_guard<std::mutex> lock(m_tpoPendingMutex);
                m_tpoPocRow = pocRow;
                m_tpoVahRow = vahRow;
                m_tpoValRow = valRow;
                m_tpoPvvGridHeight = gridHeight;
                m_tpoPvvMaxPrice = maxPrice;
                m_tpoPvvTickSize = tickSize;
                m_tpoPvvDirty = true;
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

void UnifiedGridRenderer::startHeatmapRenderLoop() {
    m_heatmapRenderTimer = new QTimer(this);
    connect(m_heatmapRenderTimer, &QTimer::timeout, this, [this]() {
        if (!m_useGpuHeatmap) return;
        auto result = m_heatmapStreamService->handleRenderTick(m_viewState.get());
        if (!result.shouldUpdate) return;
        if (result.autoScrollApplied && m_panSyncPending) {
            m_viewState->clearPanVisualOffset();
            m_panSyncPending = false;
        }
        emit liveRenderTick();
        update();
    });
    m_heatmapRenderTimer->start(16);
}
