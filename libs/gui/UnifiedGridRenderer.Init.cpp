// UnifiedGridRenderer init/data-thread wiring split from main TU.
#include "UnifiedGridRenderer.h"

#include "SentinelLogging.hpp"
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
    m_heatmapClock.start();

    const auto& store = GuiConfigStore::instance();
    if (store.hasServerConfig()) {
        const auto& server = store.serverConfig();
        if (server.heatmap.gridWidth > 0) {
            m_heatmapGridWidth = server.heatmap.gridWidth;
        }
        if (server.heatmap.gridHeight > 0) {
            m_heatmapGridHeight = server.heatmap.gridHeight;
        }
        if (server.heatmap.activeTimeframeMs > 0) {
            m_currentTimeframe_ms = server.heatmap.activeTimeframeMs;
        }
    }
    const auto& client = store.clientConfig();
    m_heatmapGamma = client.heatmap.gamma;
    m_heatmapContrast = client.heatmap.contrast;
    m_heatmapShaderFloor = client.heatmap.shaderFloor;
    if (client.heatmap.labelPx > 0) {
        m_heatmapLabelPx = client.heatmap.labelPx;
    }
    qRegisterMetaType<Trade>("Trade");
    
    m_viewState = std::make_unique<GridViewState>(this);
    m_heatmapStream = std::make_unique<HeatmapStreamState>();
    m_timeAuthority.setActiveTimeframeMs(m_currentTimeframe_ms);
    m_heatmapStream->setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
    m_heatmapStream->setAppendMs(static_cast<int>(m_currentTimeframe_ms));
    m_heatmapStream->setIntensityBytesPerCell(m_intensityBytesPerCell);
    m_heatmapOverlay.setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
    m_heatmapOverlay.setIntensityBytesPerCell(m_intensityBytesPerCell);
    m_heatmapOverlay.setBackgroundColor(m_heatmapBackgroundColor);
    m_autoScrollController = std::make_unique<ViewportAutoScrollController>();
    m_autoScrollController->setPaddingFrac(m_autoScrollPaddingFrac);
    m_autoScrollController->setSmoothEnabled(m_smoothAutoScrollEnabled);
    buildMsdfAtlas();
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessor = std::make_unique<DataProcessor>();
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    applyClientConfig(store.clientConfig());
    if (store.hasServerConfig()) {
        applyServerConfig(store.serverConfig());
    }
    
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
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this]() {
            m_dataProcessor->setHeatmapGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
            m_dataProcessor->setHeatmapIntensityScale(m_intensityScale);
        }, Qt::QueuedConnection);
        startHeatmapRenderLoop();
    }
}

bool UnifiedGridRenderer::ingestHeatmapColumnEvent(const HeatmapColumnEvent& event) {
    const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
    m_lastIncomingHeatmapSliceTimeframeMs.store(event.timeframeMs, std::memory_order_relaxed);
    if (!m_useGpuHeatmap) {
        m_useGpuHeatmap = true;
        m_heatmapOverlay.requestFullTextureRebuild();
        m_heatmapClock.start();
    }
    if (event.column.isEmpty()) {
        if (debug) {
            sLog_Render("GPU HEATMAP DROP: enabled=" << m_useGpuHeatmap
                        << " grid=" << m_heatmapGridWidth << "x" << m_heatmapGridHeight
                        << " bytes=" << event.column.size());
        }
        return false;
    }

    const int bytesPerCell = (event.intensityBytesPerCell > 0) ? event.intensityBytesPerCell : 1;
    if (bytesPerCell != m_intensityBytesPerCell) {
        m_intensityBytesPerCell = bytesPerCell;
        m_heatmapOverlay.setIntensityBytesPerCell(bytesPerCell);
        if (m_heatmapStream) {
            m_heatmapStream->setIntensityBytesPerCell(bytesPerCell);
        }
    }
    if ((event.column.size() % bytesPerCell) != 0) {
        return false;
    }
    const int columnHeight = event.column.size() / bytesPerCell;
    if (columnHeight <= 0) {
        return false;
    }
    if (columnHeight != m_heatmapGridHeight) {
        m_heatmapGridHeight = columnHeight;
        m_heatmapOverlay.setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
        if (m_heatmapStream) {
            m_heatmapStream->reset(m_heatmapGridWidth, m_heatmapGridHeight,
                                   event.minPrice, event.maxPrice, event.tickSize);
        }
        m_heatmapViewportInitialized = false;
    }

    if (debug) {
        static int debugCount = 0;
        ++debugCount;
        if (debugCount <= 5 || debugCount % 50 == 0) {
            int bidCount = 0;
            int askCount = 0;
            uint16_t minValue = std::numeric_limits<uint16_t>::max();
            uint16_t maxValue = 0;
            if (bytesPerCell == 1) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(event.column.constData());
                for (int i = 0; i < event.column.size(); ++i) {
                    const uint8_t v = bytes[i];
                    if (v == 0) {
                        continue;
                    }
                    minValue = std::min<uint16_t>(minValue, static_cast<uint16_t>(v) * 257);
                    maxValue = std::max<uint16_t>(maxValue, static_cast<uint16_t>(v) * 257);
                    if (v >= 128) {
                        ++askCount;
                    } else {
                        ++bidCount;
                    }
                }
            } else if (bytesPerCell == 2) {
                const auto* values = reinterpret_cast<const uint16_t*>(event.column.constData());
                for (int i = 0; i < columnHeight; ++i) {
                    const uint16_t v = qFromLittleEndian(values[i]);
                    if (v == 0) {
                        continue;
                    }
                    minValue = std::min(minValue, v);
                    maxValue = std::max(maxValue, v);
                    if (v >= 0x8000u) {
                        ++askCount;
                    } else {
                        ++bidCount;
                    }
                }
            }
            sLog_Render("GPU HEATMAP BYTES: bids=" << bidCount
                        << " asks=" << askCount
                        << " min=" << minValue
                        << " max=" << maxValue
                        << " bpp=" << bytesPerCell);
        }
    }

    if (event.tickSize > 0.0 && event.tickSize != m_heatmapTickSize) {
        m_heatmapTickSize = event.tickSize;
        emit heatmapTickSizeChanged();
    }

    int64_t cadenceMs = m_timeAuthority.activeTimeframeMs();
    if (cadenceMs <= 0) {
        cadenceMs = (event.timeframeMs > 0) ? event.timeframeMs : m_currentTimeframe_ms;
        m_timeAuthority.setActiveTimeframeMs(cadenceMs);
    }
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        static QElapsedTimer cadenceTimer;
        static bool cadenceTimerStarted = false;
        if (!cadenceTimerStarted) {
            cadenceTimer.start();
            cadenceTimerStarted = true;
        }
        if (cadenceTimer.elapsed() > 1000 &&
            event.timeframeMs > 0 &&
            cadenceMs > 0 &&
            event.timeframeMs != cadenceMs) {
            sLog_Debug(QString("Cadence mismatch: incoming_slice_tf=%1ms active_mapping_tf=%2ms renderer_tf=%3ms overlays[h=%4 fp=%5 tpo=%6]")
                           .arg(event.timeframeMs)
                           .arg(cadenceMs)
                           .arg(m_currentTimeframe_ms)
                           .arg(m_heatmapLayerEnabled ? 1 : 0)
                           .arg(m_footprintLayerEnabled ? 1 : 0)
                           .arg(m_tpoLayerEnabled ? 1 : 0));
            cadenceTimer.restart();
        }
    }
    if (m_heatmapStream) {
        m_heatmapStream->updateRange(event.minPrice, event.maxPrice, event.tickSize);
        if (cadenceMs > 0) {
            m_heatmapStream->setAppendMs(static_cast<int>(cadenceMs));
        }
    }

    const int expectedLiquidityBytes = m_heatmapGridHeight * static_cast<int>(sizeof(uint16_t));
    const bool haveLiquidityColumn = (event.liquidityColumn.size() == expectedLiquidityBytes);
    QByteArray intensityColumn = event.column;
    if (m_heatmapLiquidityThreshold > 0.0 && haveLiquidityColumn && event.liquidityScale > 0.0 &&
        intensityColumn.size() == m_heatmapGridHeight * bytesPerCell) {
        const auto* raw = reinterpret_cast<const uint16_t*>(event.liquidityColumn.constData());
        const double threshold = m_heatmapLiquidityThreshold;
        if (bytesPerCell == 1) {
            auto* dst = reinterpret_cast<uint8_t*>(intensityColumn.data());
            for (int y = 0; y < m_heatmapGridHeight; ++y) {
                const uint16_t packed = qFromLittleEndian(raw[y]);
                if (packed == 0) {
                    dst[y] = 0;
                    continue;
                }
                double value = static_cast<double>(packed) * event.liquidityScale;
                if (m_liquidityLabelMode != 0) {
                    const double price = event.maxPrice - (static_cast<double>(y) * event.tickSize);
                    value *= price;
                }
                if (value < threshold) {
                    dst[y] = 0;
                }
            }
        } else if (bytesPerCell == 2) {
            auto* dst = reinterpret_cast<uint16_t*>(intensityColumn.data());
            for (int y = 0; y < m_heatmapGridHeight; ++y) {
                const uint16_t packed = qFromLittleEndian(raw[y]);
                if (packed == 0) {
                    dst[y] = 0;
                    continue;
                }
                double value = static_cast<double>(packed) * event.liquidityScale;
                if (m_liquidityLabelMode != 0) {
                    const double price = event.maxPrice - (static_cast<double>(y) * event.tickSize);
                    value *= price;
                }
                if (value < threshold) {
                    dst[y] = 0;
                }
            }
        }
    }

    if (m_heatmapStream) {
        const qint64 nowMs = m_heatmapClock.elapsed();
        m_heatmapStream->ingestSlice(event.sliceStartMs,
                                     static_cast<int>(cadenceMs),
                                     intensityColumn,
                                     event.liquidityColumn,
                                     event.liquidityScale,
                                     nowMs);
        m_heatmapStream->updateTimeOffset(0.0f);
        m_timeAuthority.observeEventTime(
            (event.sliceEndMs > event.sliceStartMs)
                ? event.sliceEndMs
                : (event.sliceStartMs + cadenceMs),
            nowMs);
    }

    if (debug) {
        const int writeColumn = m_heatmapStream ? m_heatmapStream->writeColumn() : 0;
        sLog_Render("GPU HEATMAP ENQUEUE: col=" << writeColumn
                    << " tf=" << event.timeframeMs
                    << " range=$" << event.minPrice << "-$" << event.maxPrice);
    }

    if (!m_heatmapViewportInitialized && m_viewState && m_heatmapStream && m_autoScrollController) {
        if (m_autoScrollController->initializeViewport(*m_viewState,
                                                       *m_heatmapStream,
                                                       event.sliceStartMs,
                                                       static_cast<int>(cadenceMs))) {
            m_heatmapViewportInitialized = true;
            if (debug) {
                const auto snapshot = m_heatmapStream->snapshot();
                sLog_Render("GPU HEATMAP VIEWPORT INIT: [" << m_viewState->getVisibleTimeStart()
                            << "-" << m_viewState->getVisibleTimeEnd()
                            << "] $" << snapshot.minPrice << "-$" << snapshot.maxPrice);
            }
        }
    }

    if (m_viewState && m_viewState->isAutoScrollEnabled() && m_heatmapStream && m_autoScrollController &&
        !m_autoScrollController->smoothEnabled()) {
        const bool applied = m_autoScrollController->applySliceAutoScroll(*m_viewState,
                                                                          *m_heatmapStream,
                                                                          event.sliceStartMs,
                                                                          static_cast<int>(cadenceMs));
        if (applied && m_panSyncPending) {
            m_viewState->clearPanVisualOffset();
            m_panSyncPending = false;
        }
    }

    m_heatmapStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void UnifiedGridRenderer::connectDataProcessorSignals() {
    connect(m_dataProcessor.get(), &DataProcessor::heatmapColumnReady,
            this,
            [this](int64_t sliceStartMs,
                   int64_t sliceEndMs,
                   int64_t timeframeMs,
                   double minPrice,
                   double maxPrice,
                   double tickSize,
                   const QByteArray& column,
                   const QByteArray& liquidityColumn,
                   double liquidityScale,
                   int intensityBytesPerCell) {
                HeatmapColumnEvent event;
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
                if (ingestHeatmapColumnEvent(event)) {
                    update();
                }
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::heatmapHistoryBatchReady,
            this,
            [this](int64_t timeframeMs,
                   int gridWidth,
                   int gridHeight,
                   const QVector<IGridDataSource::HeatmapHistoryColumn>& columns,
                   int intensityBytesPerCell) {
                Q_UNUSED(gridWidth);
                Q_UNUSED(gridHeight);
                bool anyApplied = false;
                for (const auto& col : columns) {
                    HeatmapColumnEvent event;
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
                    anyApplied = ingestHeatmapColumnEvent(event) || anyApplied;
                }
                if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                    sLog_Debug(QString("Heatmap history batch ingested: columns=%1 queued_signals=1")
                                   .arg(columns.size()));
                }
                if (anyApplied) {
                    update();
                }
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::heatmapRangeReset,
            this,
            [this](double minPrice, double maxPrice, double tickSize, int gridWidth, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapOverlay.requestFullTextureRebuild();
                    m_heatmapClock.start();
                }
                if (gridWidth > 0) {
                    m_heatmapGridWidth = gridWidth;
                }
                if (gridHeight > 0) {
                    m_heatmapGridHeight = gridHeight;
                }
                m_heatmapOverlay.setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
                m_heatmapOverlay.requestFullTextureRebuild();
                m_heatmapStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
                if (tickSize > 0.0 && tickSize != m_heatmapTickSize) {
                    m_heatmapTickSize = tickSize;
                    emit heatmapTickSizeChanged();
                }
                if (m_heatmapStream) {
                    m_heatmapStream->reset(m_heatmapGridWidth, m_heatmapGridHeight,
                                           minPrice, maxPrice, tickSize);
                }
                if (m_autoScrollController) {
                    m_autoScrollController->resetSpan();
                }
                if (m_viewState && m_viewState->isAutoScrollEnabled()) {
                    m_heatmapViewportInitialized = false;
                }
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::footprintColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray columnQ16) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapClock.start();
                }
                if (gridWidth <= 0 || gridHeight <= 0) {
                    return;
                }
                if (gridHeight > (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)))) {
                    return;
                }
                const int expectedBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
                if (columnQ16.size() != expectedBytes) {
                    return;
                }
                if (x < 0 || x >= gridWidth) {
                    return;
                }

                int pendingCount = 0;
                {
                    std::lock_guard<std::mutex> lock(m_footprintPendingMutex);
                    if (m_pendingFootprintUploads.capacity() < static_cast<size_t>(gridWidth)) {
                        m_pendingFootprintUploads.reserve(static_cast<size_t>(gridWidth));
                    }
                    m_pendingFootprintUploads.push_back(
                        FootprintOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(columnQ16)});
                    pendingCount = static_cast<int>(m_pendingFootprintUploads.size());
                }
                m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
                if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
                    sLog_Debug(QString("Footprint queued for render: x=%1 grid=%2x%3 pending=%4")
                                   .arg(x)
                                   .arg(gridWidth)
                                   .arg(gridHeight)
                                   .arg(pendingCount));
                }
                update();
            },
            Qt::QueuedConnection);

    connect(m_dataProcessor.get(), &DataProcessor::tpoColumnReady,
            this,
            [this](int x, int gridWidth, int gridHeight, QByteArray letters) {
                if (gridWidth <= 0 || gridHeight <= 0 || x < 0 || x >= gridWidth) {
                    return;
                }
                if (letters.size() != gridHeight) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_tpoPendingMutex);
                    if (m_pendingTpoUploads.capacity() < static_cast<size_t>(gridWidth)) {
                        m_pendingTpoUploads.reserve(static_cast<size_t>(gridWidth));
                    }
                    m_pendingTpoUploads.push_back(
                        TpoOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(letters)});
                }
                update();
            },
            Qt::QueuedConnection);

    // ── Volume Profile (Mode A) ─────────────────────────────────────────────
    // volumeProfileReady is emitted on the DataProcessor thread; we capture
    // the bins and snapshot under a mutex then schedule a repaint.
    connect(m_dataProcessor.get(), &DataProcessor::volumeProfileReady,
            this,
            [this](std::vector<float> bins, VolumeProfileState::Snapshot snap) {
                if (bins.empty()) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_vpMutex);
                    m_vpBins  = std::move(bins);
                    m_vpSnap  = std::move(snap);
                    m_vpDirty = true;
                }
                update();
            },
            Qt::QueuedConnection);
}

void UnifiedGridRenderer::startHeatmapRenderLoop() {
    m_heatmapRenderTimer = new QTimer(this);
    connect(m_heatmapRenderTimer, &QTimer::timeout, this, [this]() {
        const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
        const qint64 nowMs = m_heatmapClock.elapsed();
        const auto timeSnapshot = m_timeAuthority.snapshot(nowMs);
        const int64_t cadenceMs = (timeSnapshot.activeTimeframeMs > 0)
            ? timeSnapshot.activeTimeframeMs
            : static_cast<int64_t>(snapshot.appendMs);
        if (!m_useGpuHeatmap || snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0 ||
            cadenceMs <= 0) {
            return;
        }
        const bool dragging = (m_viewState && m_viewState->isDragging());
        const qint64 lastAppendMs = m_heatmapStream ? m_heatmapStream->lastAppendMs() : 0;
        const qint64 delta = nowMs - lastAppendMs;
        const bool useFractionalOffset = (m_viewState && m_viewState->isAutoScrollEnabled() &&
                                          m_autoScrollController && !m_autoScrollController->smoothEnabled());
        const float frac = useFractionalOffset
            ? std::clamp(static_cast<float>(delta) / static_cast<float>(cadenceMs), 0.0f, 1.0f)
            : 0.0f;
        if (m_heatmapStream) {
            m_heatmapStream->updateTimeOffset(frac);
        }
        if (!dragging && m_autoScrollController && m_autoScrollController->smoothEnabled() &&
            m_viewState && m_viewState->isAutoScrollEnabled() && m_heatmapStream) {
            const bool applied = m_autoScrollController->applySmoothAutoScroll(*m_viewState,
                                                                               *m_heatmapStream,
                                                                               nowMs,
                                                                               cadenceMs);
            if (applied && m_panSyncPending) {
                m_viewState->clearPanVisualOffset();
                m_panSyncPending = false;
            }
        }
        update();
    });
    m_heatmapRenderTimer->start(16);
}


