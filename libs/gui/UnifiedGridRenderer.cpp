// Slots on main thread, paint on render thread.
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QThread>
#include <QMetaObject>
#include <QMetaType>
#include <QTimer>
#include <QtEndian>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include "render/GridViewState.hpp"
#include "render/DataProcessor.hpp"
#include "render/MsdfAtlas.hpp"
#include "render/MsdfGlyphNode.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/HeatmapStreamState.hpp"
#include "render/ViewportAutoScrollController.hpp"
#include "render/HeatmapLabelRenderer.hpp"
#include "render/UgrFrameMath.hpp"
#include "config/GuiConfigStore.hpp"

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    setAcceptHoverEvents(false);  // Reduce event capture

    init();
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
    if (m_dataProcessor) {
        if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::stopProcessing, Qt::BlockingQueuedConnection);
        } else {
            m_dataProcessor->stopProcessing();
        }
        disconnect(m_dataProcessor.get(), nullptr, this, nullptr);
    }

    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
        m_dataProcessorThread->quit();
        if (!m_dataProcessorThread->wait(5000)) {
            m_dataProcessorThread->terminate();
            m_dataProcessorThread->wait(1000);
        }
    }

    m_dataProcessor.reset();
    m_dataProcessorThread.reset();
}

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    Q_UNUSED(trade);
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    if (m_viewState) {
        m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
    }
    
    update();
    
    sLog_Debug("UNIFIED RENDERER VIEWPORT Time:[" << startTimeMs << "-" << endTimeMs << "]"
               << "Price:[$" << minPrice << "-$" << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
    if (!m_viewState || !m_dataProcessor) return;
    update();
}


void UnifiedGridRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    
    if (newGeometry.size() != oldGeometry.size()) {
        sLog_Render("UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        if (m_viewState) {
            m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
        }
        update();
    }
}

void UnifiedGridRenderer::componentComplete() {
    QQuickItem::componentComplete();

    if (m_viewState && width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
    }
}

void UnifiedGridRenderer::setIntensityScale(double scale) {
    if (m_intensityScale != scale) {
        m_intensityScale = scale;
        if (m_useGpuHeatmap && m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, scale]() {
                m_dataProcessor->setHeatmapIntensityScale(scale);
            }, Qt::QueuedConnection);
        }
        update();
        emit intensityScaleChanged();
    }
}

void UnifiedGridRenderer::setMaxCells(int max) {
    if (m_maxCells != max) {
        m_maxCells = max;
        emit maxCellsChanged();
    }
}

void UnifiedGridRenderer::setMinVolumeFilter(double minVolume) {
    if (m_minVolumeFilter != minVolume) {
        m_minVolumeFilter = minVolume;
        update();
        emit minVolumeFilterChanged();
    }
}

void UnifiedGridRenderer::setAutoScrollPaddingFrac(double fraction) {
    const double clamped = std::clamp(fraction, 0.0, 0.45);
    if (m_autoScrollPaddingFrac != clamped) {
        m_autoScrollPaddingFrac = clamped;
        if (m_autoScrollController) {
            m_autoScrollController->setPaddingFrac(clamped);
            m_autoScrollController->resetSpan();
        }
        emit autoScrollPaddingFracChanged();
    }
}

void UnifiedGridRenderer::setAutoScrollSmoothEnabled(bool enabled) {
    if (m_smoothAutoScrollEnabled != enabled) {
        m_smoothAutoScrollEnabled = enabled;
        if (m_autoScrollController) {
            m_autoScrollController->setSmoothEnabled(enabled);
        }
        emit autoScrollSmoothEnabledChanged();
    }
}

void UnifiedGridRenderer::setShowGpuStatsOverlay(bool show) {
    if (m_showGpuStatsOverlay != show) {
        m_showGpuStatsOverlay = show;
        emit showGpuStatsOverlayChanged();
    }
}

void UnifiedGridRenderer::setShowDataPipelineOverlay(bool show) {
    if (m_showDataPipelineOverlay != show) {
        m_showDataPipelineOverlay = show;
        emit showDataPipelineOverlayChanged();
    }
}

void UnifiedGridRenderer::setShowRenderStrategyOverlay(bool show) {
    if (m_showRenderStrategyOverlay != show) {
        m_showRenderStrategyOverlay = show;
        emit showRenderStrategyOverlayChanged();
    }
}

void UnifiedGridRenderer::setShowViewportMathOverlay(bool show) {
    if (m_showViewportMathOverlay != show) {
        m_showViewportMathOverlay = show;
        emit showViewportMathOverlayChanged();
    }
}

void UnifiedGridRenderer::setShowMemoryCacheOverlay(bool show) {
    if (m_showMemoryCacheOverlay != show) {
        m_showMemoryCacheOverlay = show;
        emit showMemoryCacheOverlayChanged();
    }
}

void UnifiedGridRenderer::setShowModeFlagsOverlay(bool show) {
    if (m_showModeFlagsOverlay != show) {
        m_showModeFlagsOverlay = show;
        emit showModeFlagsOverlayChanged();
    }
}

void UnifiedGridRenderer::clearData() {
    if (m_viewState) {
        m_viewState->resetZoom();
    }
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::clearData, Qt::QueuedConnection);
    }
    {
        std::lock_guard<std::mutex> lock(m_footprintPendingMutex);
        m_pendingFootprintUploads.clear();
    }
    m_footprintOverlay.requestNeutralReset();
    m_heatmapStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_footprintStreamGeneration.fetch_add(1, std::memory_order_acq_rel);
    update();
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_dataProcessor && resolution > 0) {
        QMetaObject::invokeMethod(m_dataProcessor.get(),
                                  [this, resolution]() { m_dataProcessor->setPriceResolution(resolution); },
                                  Qt::QueuedConnection);
        update();
    }
}

void UnifiedGridRenderer::setGridResolutionPreset(int preset) {
    const double priceRes[] = {2.5, 5.0, 10.0};
    const int timeRes[] = {50, 100, 250};
    if (preset >= 0 && preset <= 2) {
        setPriceResolution(priceRes[preset]);
        setTimeframe(timeRes[preset]);
    }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
    if (m_currentTimeframe_ms != timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        m_timeAuthority.setActiveTimeframeMs(static_cast<int64_t>(timeframe_ms));
        if (m_useGpuHeatmap && timeframe_ms > 0) {
            if (m_heatmapStream) {
                m_heatmapStream->setAppendMs(timeframe_ms);
            }
        }
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        if (m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(),
                                      [this, timeframe_ms]() { m_dataProcessor->addTimeframe(timeframe_ms); },
                                      Qt::QueuedConnection);
        }
        update();
        emit timeframeChanged();
    }
}

void UnifiedGridRenderer::setLiquidityLabelMode(int mode) {
    if (m_liquidityLabelMode == mode) {
        return;
    }
    m_liquidityLabelMode = mode;
    update();
    emit liquidityLabelModeChanged();
}

void UnifiedGridRenderer::setHeatmapLiquidityThreshold(double threshold) {
    const double clamped = std::max(0.0, threshold);
    if (std::abs(m_heatmapLiquidityThreshold - clamped) < 1e-9) {
        return;
    }
    m_heatmapLiquidityThreshold = clamped;
    update();
    emit heatmapLiquidityThresholdChanged();
}

void UnifiedGridRenderer::setHeatmapBackgroundColor(const QColor& color) {
    if (m_heatmapBackgroundColor == color) {
        return;
    }
    m_heatmapBackgroundColor = color;
    m_heatmapOverlay.setBackgroundColor(color);
    emit heatmapBackgroundColorChanged();
}

void UnifiedGridRenderer::setHeatmapGamma(double gamma) {
    const double clamped = std::clamp(gamma, 0.1, 5.0);
    if (std::abs(m_heatmapGamma - clamped) < 1e-6) {
        return;
    }
    m_heatmapGamma = clamped;
    update();
    emit heatmapGammaChanged();
}

void UnifiedGridRenderer::setHeatmapContrast(double contrast) {
    const double clamped = std::clamp(contrast, 0.1, 5.0);
    if (std::abs(m_heatmapContrast - clamped) < 1e-6) {
        return;
    }
    m_heatmapContrast = clamped;
    update();
    emit heatmapContrastChanged();
}

void UnifiedGridRenderer::setHeatmapShaderFloor(double floor) {
    const double clamped = std::clamp(floor, 0.0, 0.5);
    if (std::abs(m_heatmapShaderFloor - clamped) < 1e-6) {
        return;
    }
    m_heatmapShaderFloor = clamped;
    update();
    emit heatmapShaderFloorChanged();
}

void UnifiedGridRenderer::setPrimaryField(int field) {
    if (m_primaryField == field) {
        return;
    }
    m_primaryField = field;
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Render("PrimaryField set to " << m_primaryField);
    }
    update();
    emit primaryFieldChanged();
}

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        update();
        emit autoScrollEnabledChanged();
        sLog_Render("Auto-scroll: "<< (enabled ? "ENABLED" : "DISABLED"));
        if (enabled && m_viewState->isTimeWindowValid() && m_heatmapStream && m_autoScrollController) {
            m_autoScrollController->updateLagFromView(*m_viewState,
                                                      *m_heatmapStream,
                                                      m_timeAuthority.activeTimeframeMs());
        }
    }
}

//  COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport;
    viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
    viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
    viewport.priceMin = m_viewState->getMinPrice();
    viewport.priceMax = m_viewState->getMaxPrice();
    viewport.width = width();
    viewport.height = height();
    return CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
}

QPointF UnifiedGridRenderer::screenToWorld(double screenX, double screenY) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport;
    viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
    viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
    viewport.priceMin = m_viewState->getMinPrice();
    viewport.priceMax = m_viewState->getMaxPrice();
    viewport.width = width();
    viewport.height = height();
    return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
}

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

UnifiedGridRenderer::FrameContext UnifiedGridRenderer::buildFrameContext() const {
    FrameContext frame;
    frame.surfaceBounds = boundingRect();
    frame.surfaceDpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qint64 steadyNowMs = m_heatmapClock.isValid() ? m_heatmapClock.elapsed() : 0;
    frame.time = m_timeAuthority.snapshot(steadyNowMs);
    frame.presentationTimeMs = frame.time.nowPresentationMs;
    frame.heatmapSnapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    frame.overlays.heatmap = (m_primaryField == 0);
    frame.overlays.footprint = (m_primaryField == 1);
    frame.forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
    frame.streamGenerations.heatmap = m_heatmapStreamGeneration.load(std::memory_order_acquire);
    frame.streamGenerations.footprint = m_footprintStreamGeneration.load(std::memory_order_acquire);
    frame.streamGenerations.candle = m_candleStreamGeneration.load(std::memory_order_acquire);
    if (m_viewState && m_viewState->isTimeWindowValid()) {
        frame.viewport.valid = true;
        frame.viewport.timeStart = m_viewState->getVisibleTimeStart();
        frame.viewport.timeEnd = m_viewState->getVisibleTimeEnd();
        frame.viewport.minPrice = m_viewState->getMinPrice();
        frame.viewport.maxPrice = m_viewState->getMaxPrice();
        frame.viewport.panVisualOffset = m_viewState->getPanVisualOffset();
        frame.viewport.dragging = m_viewState->isDragging();
        frame.viewport.autoScrollEnabled = m_viewState->isAutoScrollEnabled();
    }
    return frame;
}

HeatmapIntensityNode* UnifiedGridRenderer::ensureHeatmapRootNode(QSGNode* oldNode) {
    auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
    if (!texNode) {
        texNode = new HeatmapIntensityNode();
        m_heatmapOverlay.onRootRebuilt();
        m_whiteGlyphNode = nullptr;
        m_blackGlyphNode = nullptr;
        m_footprintOverlay.onRootRebuilt();
    }
    return texNode;
}

void UnifiedGridRenderer::computeAndApplyFrameMapping(FrameContext& frame,
                                                       HeatmapIntensityNode* texNode,
                                                       int64_t cadenceMs,
                                                       int gridWidth,
                                                       int gridHeight) {
    const auto& snapshot = frame.heatmapSnapshot;
    m_heatmapOverlay.setGridDimensions(gridWidth, gridHeight);
    m_heatmapOverlay.setIntensityBytesPerCell(m_intensityBytesPerCell);
    m_heatmapOverlay.setBackgroundColor(m_heatmapBackgroundColor);

    const QRectF bounds = frame.surfaceBounds;
    UgrFrameMath::ViewportState viewportState;
    viewportState.valid = frame.viewport.valid;
    viewportState.timeStart = static_cast<double>(frame.viewport.timeStart);
    viewportState.timeEnd = static_cast<double>(frame.viewport.timeEnd);
    viewportState.minPrice = frame.viewport.minPrice;
    viewportState.maxPrice = frame.viewport.maxPrice;
    viewportState.panVisualOffset = frame.viewport.panVisualOffset;
    viewportState.dragging = frame.viewport.dragging;
    viewportState = UgrFrameMath::applyDragPan(viewportState, bounds);

    UgrFrameMath::GridState gridState;
    gridState.gridWidth = gridWidth;
    gridState.gridHeight = gridHeight;
    gridState.cadenceMs = cadenceMs;
    gridState.timeOriginMs = snapshot.timeOriginMs;
    gridState.lastSliceStartMs = snapshot.lastSliceStartMs;
    gridState.filledColumns = snapshot.filledColumns;
    gridState.tickSize = snapshot.tickSize;
    gridState.dataMinPrice = snapshot.minPrice;
    gridState.dataMaxPrice = snapshot.maxPrice;
    gridState.forceFull = frame.forceFull;

    const UgrFrameMath::RenderRects renderRects =
        UgrFrameMath::computeRenderRects(bounds, viewportState, gridState);
    texNode->setRect(renderRects.drawRect);
    texNode->setSourceRect(renderRects.srcRect);
    if (!frame.forceFull) {
        texNode->setTimeOffset(snapshot.timeOffset);
    }

    frame.mapping.viewStartMs = renderRects.viewTimeStart;
    frame.mapping.viewEndMs = renderRects.viewTimeEnd;
    frame.mapping.viewMinPrice = renderRects.viewMinPrice;
    frame.mapping.viewMaxPrice = renderRects.viewMaxPrice;
    frame.mapping.drawRect = renderRects.drawRect;
    frame.mapping.srcRect = renderRects.srcRect;
    frame.mapping.dataStartMs = renderRects.dataStart;
    frame.mapping.dataEndMs = renderRects.dataEnd;
    frame.mapping.actualDataStartMs = renderRects.actualDataStart;
    frame.mapping.actualDataEndMs = renderRects.actualDataEnd;
    frame.mapping.dataMinPrice = snapshot.minPrice;
    frame.mapping.dataMaxPrice = snapshot.maxPrice;
    frame.mapping.appendMs = static_cast<double>(cadenceMs);
    frame.mapping.tickSize = snapshot.tickSize;
    frame.mapping.gridWidth = gridWidth;
    frame.mapping.gridHeight = gridHeight;
    frame.mapping.filledColumns = snapshot.filledColumns;
    frame.mapping.timeOffset = frame.forceFull ? 0.0f : snapshot.timeOffset;
    frame.mapping.valid = (renderRects.dataStartValid &&
                           snapshot.timeOriginMs != 0 &&
                           cadenceMs > 0 &&
                           gridWidth > 0 &&
                           renderRects.drawRect.width() > 0.0 &&
                           renderRects.srcRect.width() > 0.0);
    frame.mapping.cellW = frame.mapping.valid
        ? (renderRects.drawRect.width() / renderRects.srcRect.width()) : 0.0;
    frame.mapping.cellH = frame.mapping.valid && renderRects.srcRect.height() > 0.0
        ? (renderRects.drawRect.height() / renderRects.srcRect.height()) : 0.0;
    m_lastTimeAxisMapping = frame.mapping;
}

void UnifiedGridRenderer::publishFrameContext(const FrameContext& frame) {
    MappingFrameContext published;
    published.surfaceBounds = frame.surfaceBounds;
    published.surfaceDpr = frame.surfaceDpr;
    published.presentationTimeMs = frame.presentationTimeMs;
    published.activeTimeframeMs = frame.time.activeTimeframeMs;
    published.nowEventTimeMs = frame.time.nowEventMs;
    published.currentBoundaryStartMs = frame.time.currentBoundaryStartMs;
    published.nextBoundaryStartMs = frame.time.nextBoundaryStartMs;
    published.boundarySequence = frame.time.boundarySequence;
    published.hasEventTime = frame.time.hasEvent;
    published.viewportValid = frame.viewport.valid;
    published.viewportTimeStart = frame.viewport.timeStart;
    published.viewportTimeEnd = frame.viewport.timeEnd;
    published.viewportMinPrice = frame.viewport.minPrice;
    published.viewportMaxPrice = frame.viewport.maxPrice;
    published.viewportPanVisualOffset = frame.viewport.panVisualOffset;
    published.viewportDragging = frame.viewport.dragging;
    published.viewportAutoScrollEnabled = frame.viewport.autoScrollEnabled;
    published.heatmapGeneration = frame.streamGenerations.heatmap;
    published.footprintGeneration = frame.streamGenerations.footprint;
    published.candleGeneration = frame.streamGenerations.candle;
    published.mapping = frame.mapping;
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    m_lastFrameContext = published;
}

void UnifiedGridRenderer::drainFrameUploads(
    std::vector<HeatmapOverlayRenderer::PendingUpload>& heatmapUploads,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads) {
    {
        std::lock_guard<std::mutex> lock(m_footprintPendingMutex);
        if (!m_pendingFootprintUploads.empty()) {
            footprintUploads.swap(m_pendingFootprintUploads);
        }
    }

    if (!m_heatmapStream) {
        return;
    }
    std::vector<HeatmapStreamState::PendingColumn> pendingUploads;
    m_heatmapStream->takePendingUploads(pendingUploads);
    heatmapUploads.reserve(pendingUploads.size());
    for (auto& upload : pendingUploads) {
        heatmapUploads.push_back({upload.x, std::move(upload.data)});
    }
}

void UnifiedGridRenderer::renderOverlays(
    HeatmapIntensityNode* texNode,
    const FrameContext& frame,
    bool drawHeatmap,
    bool drawFootprint,
    int gridWidth,
    int gridHeight,
    std::vector<HeatmapOverlayRenderer::PendingUpload>& heatmapUploads,
    std::vector<FootprintOverlayRenderer::PendingUpload>& footprintUploads) {
    const auto& snapshot = frame.heatmapSnapshot;
    const QRectF drawRect = frame.mapping.drawRect;
    const QRectF srcRect = frame.mapping.srcRect;
    m_heatmapOverlay.applyToNode(window(),
                                 texNode,
                                 drawHeatmap,
                                 static_cast<float>(m_heatmapGamma),
                                 static_cast<float>(m_heatmapContrast),
                                 static_cast<float>(m_heatmapShaderFloor),
                                 frame.forceFull,
                                 snapshot.timeOffset,
                                 drawRect,
                                 srcRect,
                                 heatmapUploads);
    m_footprintOverlay.render(window(),
                              texNode,
                              drawFootprint,
                              frame.forceFull,
                              snapshot.timeOffset,
                              drawRect,
                              srcRect,
                              gridWidth,
                              gridHeight,
                              footprintUploads);
}

void UnifiedGridRenderer::updateLabelGeometry(HeatmapIntensityNode* texNode,
                                              const FrameContext& frame,
                                              const HeatmapStreamState::Snapshot& snapshot,
                                              int gridWidth,
                                              int gridHeight) {
    HeatmapStreamState::LabelSnapshot labelSnapshot;
    const bool haveLabelSnapshot = m_heatmapStream && m_heatmapStream->copyLabelSnapshot(labelSnapshot);
    const bool labelGridMatches = haveLabelSnapshot &&
                                  labelSnapshot.snapshot.gridWidth == gridWidth &&
                                  labelSnapshot.snapshot.gridHeight == gridHeight;

    const QRectF drawRect = frame.mapping.drawRect;
    const QRectF srcRectCurrent = texNode->getSourceRect();
    const bool labelVisible = (!drawRect.isEmpty() && !frame.surfaceBounds.isEmpty() &&
                               srcRectCurrent.width() > 0.0 && srcRectCurrent.height() > 0.0 &&
                               labelGridMatches);
    const float cellH = (srcRectCurrent.height() > 0.0f)
        ? static_cast<float>(drawRect.height()) / static_cast<float>(srcRectCurrent.height())
        : 0.0f;
    const int labelPx = (m_heatmapLabelPx > 0) ? m_heatmapLabelPx : 14;
    const float labelThreshold = static_cast<float>(labelPx);

    if (!(labelVisible && cellH >= labelThreshold && m_msdfAtlasBuilt && window())) {
        clearLabelGeometry();
        return;
    }

    const float fontPx = static_cast<float>(m_msdfAtlas.fontPx());
    const float scale = (fontPx > 0.0f) ? std::clamp(cellH / fontPx, 0.25f, 2.5f) : 1.0f;

    if (m_labelWhiteQuads.capacity() < 32000) {
        m_labelWhiteQuads.reserve(32000);
    }
    if (m_labelBlackQuads.capacity() < 32000) {
        m_labelBlackQuads.reserve(32000);
    }

    const bool dollars = (m_liquidityLabelMode != 0);
    HeatmapLabelRenderer::buildLabelQuads(frame.mapping,
                                          snapshot,
                                          m_msdfAtlas,
                                          labelSnapshot.liquidityRing,
                                          labelSnapshot.intensityRing,
                                          labelSnapshot.liquidityScales,
                                          scale,
                                          dollars,
                                          m_labelWhiteQuads,
                                          m_labelBlackQuads);

    if (!m_whiteGlyphNode) {
        m_whiteGlyphNode = new MsdfGlyphNode();
        m_whiteGlyphNode->setColor(Qt::white);
        m_whiteGlyphNode->ensureCapacity(32000);
        texNode->appendChildNode(m_whiteGlyphNode);
    }
    if (!m_blackGlyphNode) {
        m_blackGlyphNode = new MsdfGlyphNode();
        m_blackGlyphNode->setColor(Qt::black);
        m_blackGlyphNode->ensureCapacity(32000);
        texNode->appendChildNode(m_blackGlyphNode);
    }

    m_whiteGlyphNode->setAtlas(m_msdfAtlas.image(), window());
    m_whiteGlyphNode->setPxRange(m_msdfAtlas.pxRange());
    m_blackGlyphNode->setAtlas(m_msdfAtlas.image(), window());
    m_blackGlyphNode->setPxRange(m_msdfAtlas.pxRange());
    m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
    m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
}

void UnifiedGridRenderer::clearLabelGeometry() {
    if (m_whiteGlyphNode) {
        m_labelWhiteQuads.clear();
        m_whiteGlyphNode->updateGeometry(m_labelWhiteQuads);
    }
    if (m_blackGlyphNode) {
        m_labelBlackQuads.clear();
        m_blackGlyphNode->updateGeometry(m_labelBlackQuads);
    }
}

void UnifiedGridRenderer::updateFpsEstimate() {
    if (!m_fpsTimer.isValid()) {
        m_fpsTimer.start();
        m_fpsFrameCount = 0;
    }

    ++m_fpsFrameCount;
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    if (elapsedMs >= 1000) {
        m_currentFps.store((static_cast<double>(m_fpsFrameCount) * 1000.0) / elapsedMs);
        m_fpsFrameCount = 0;
        m_fpsTimer.restart();
    }
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0 || !m_useGpuHeatmap) {
        return oldNode;
    }

    FrameContext frame = buildFrameContext();
    const auto& snapshot = frame.heatmapSnapshot;
    const int64_t cadenceMs = (frame.time.activeTimeframeMs > 0)
        ? frame.time.activeTimeframeMs
        : static_cast<int64_t>(snapshot.appendMs);
    const bool drawHeatmap = frame.overlays.heatmap;
    const bool drawFootprint = frame.overlays.footprint;
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;

    auto* texNode = ensureHeatmapRootNode(oldNode);
    computeAndApplyFrameMapping(frame, texNode, cadenceMs, gridWidth, gridHeight);
    publishFrameContext(frame);

    std::vector<HeatmapOverlayRenderer::PendingUpload> framePendingHeatmapUploads;
    std::vector<FootprintOverlayRenderer::PendingUpload> framePendingFootprintUploads;
    drainFrameUploads(framePendingHeatmapUploads, framePendingFootprintUploads);
    renderOverlays(texNode,
                   frame,
                   drawHeatmap,
                   drawFootprint,
                   gridWidth,
                   gridHeight,
                   framePendingHeatmapUploads,
                   framePendingFootprintUploads);

    if (!drawHeatmap) {
        m_whiteGlyphNode = nullptr;
        m_blackGlyphNode = nullptr;
        return texNode;
    }

    updateLabelGeometry(texNode, frame, snapshot, gridWidth, gridHeight);
    updateFpsEstimate();
    return texNode;
}

void UnifiedGridRenderer::buildMsdfAtlas() {
    if (m_msdfAtlasBuilt) {
        return;
    }
    MsdfAtlas::BuildParams params;
    params.fontFamily = "Roboto Mono";
    params.fontPx = 64;
    params.pxRange = 8.0f;
    params.charset = QStringLiteral("0123456789.kMB+-");
    const QByteArray envFont = qgetenv("SENTINEL_MSDF_FONT");
    if (!envFont.isEmpty()) {
        params.fontPath = QString::fromUtf8(envFont);
    } else {
        const auto& client = GuiConfigStore::instance().clientConfig();
        if (!client.gui.msdfFontPath.empty()) {
            params.fontPath = QString::fromStdString(client.gui.msdfFontPath);
        }
    }
    if (m_msdfAtlas.build(params)) {
        if (qEnvironmentVariableIsSet("SENTINEL_DUMP_GLYPH_ATLAS")) {
            m_msdfAtlas.image().save("/tmp/sentinel_msdf_atlas.png");
        }
        m_msdfAtlasBuilt = true;
    }
}

void UnifiedGridRenderer::applyClientConfig(const ClientConfig& config) {
    setHeatmapGamma(config.heatmap.gamma);
    setHeatmapContrast(config.heatmap.contrast);
    setHeatmapShaderFloor(config.heatmap.shaderFloor);
    if (config.heatmap.labelPx > 0) {
        m_heatmapLabelPx = config.heatmap.labelPx;
    }
    update();
    if (m_dataProcessor && config.heatmap.clientCacheColumns > 0) {
        const int capacity = config.heatmap.clientCacheColumns;
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this, capacity]() {
            m_dataProcessor->setCacheCapacityOverride(capacity);
        }, Qt::QueuedConnection);
    }
}

void UnifiedGridRenderer::applyServerConfig(const ServerConfig& config) {
    bool gridChanged = false;
    if (config.heatmap.gridWidth > 0 && config.heatmap.gridWidth != m_heatmapGridWidth) {
        m_heatmapGridWidth = config.heatmap.gridWidth;
        gridChanged = true;
    }
    if (config.heatmap.gridHeight > 0 && config.heatmap.gridHeight != m_heatmapGridHeight) {
        m_heatmapGridHeight = config.heatmap.gridHeight;
        gridChanged = true;
    }
    if (gridChanged) {
        m_heatmapOverlay.setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
        m_heatmapOverlay.requestFullTextureRebuild();
        if (m_heatmapStream) {
            m_heatmapStream->setGridDimensions(m_heatmapGridWidth, m_heatmapGridHeight);
        }
    }

    int64_t forcedTf = config.heatmap.activeTimeframeMs;
    if (forcedTf <= 0 && !config.heatmap.timeframesMs.empty()) {
        forcedTf = config.heatmap.timeframesMs.front();
    }
    if (forcedTf > 0) {
        setTimeframe(static_cast<int>(forcedTf));
        if (m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, forcedTf]() {
                m_dataProcessor->setServerTimeframe(forcedTf);
            }, Qt::QueuedConnection);
        }
    }

    if (m_dataProcessor && config.heatmap.recenterDelta > 0.0) {
        const double recenter = config.heatmap.recenterDelta;
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this, recenter]() {
            m_dataProcessor->setHeatmapRecenterFraction(recenter);
        }, Qt::QueuedConnection);
    }
}

void UnifiedGridRenderer::fitHeatmapToDataRange() {
    const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
        ? m_timeAuthority.activeTimeframeMs()
        : static_cast<int64_t>(snapshot.appendMs);
    if (!m_viewState || cadenceMs <= 0 || snapshot.gridWidth <= 0) {
        return;
    }
    if (snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return;
    }
    const int64_t bufferSpanMs = std::max<int64_t>(
        1, static_cast<int64_t>(snapshot.gridWidth) * cadenceMs);
    const int64_t dataEnd = snapshot.lastSliceStartMs + cadenceMs;
    const int64_t dataStart = dataEnd - bufferSpanMs;
    if (dataEnd <= dataStart) {
        return;
    }
    if (m_viewState->isAutoScrollEnabled()) {
        m_viewState->enableAutoScroll(false);
    }
    m_viewState->setViewport(dataStart, dataEnd, snapshot.minPrice, snapshot.maxPrice);
    update();
}

QString UnifiedGridRenderer::getTextureSize() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth > 0 && snapshot.gridHeight > 0) {
            return QString("%1x%2").arg(snapshot.gridWidth).arg(snapshot.gridHeight);
        }
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0) {
            return "N/A";
        }
        const int bytesPerPixel = (m_intensityBytesPerCell > 0) ? m_intensityBytesPerCell : 1;
        qint64 bytes = static_cast<qint64>(snapshot.gridWidth) * snapshot.gridHeight * bytesPerPixel;
        double mb = bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
    if (m_useGpuHeatmap) {
        return (m_intensityBytesPerCell == 2) ? "Grayscale16" : "Grayscale8";
    }
    return "N/A";
}

QString UnifiedGridRenderer::getLabelRingMemory() const {
    if (!m_heatmapStream) {
        return "Label ring: N/A";
    }
    HeatmapStreamState::LabelSnapshot labels;
    if (!m_heatmapStream->copyLabelSnapshot(labels)) {
        return "Label ring: N/A";
    }
    const int gridWidth = labels.snapshot.gridWidth;
    const int gridHeight = labels.snapshot.gridHeight;
    if (gridWidth <= 0 || gridHeight <= 0) {
        return "Label ring: N/A";
    }
    const qint64 cells = static_cast<qint64>(gridWidth) * gridHeight;
    const qint64 bytesIntensity = cells * static_cast<qint64>(sizeof(uint16_t));
    const qint64 bytesLiquidity = cells * static_cast<qint64>(sizeof(uint16_t));
    const qint64 bytesScales = static_cast<qint64>(gridWidth) * static_cast<qint64>(sizeof(double));
    const qint64 totalBytes = bytesIntensity + bytesLiquidity + bytesScales;
    const double mb = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    return QString("Label ring: %1 MB").arg(mb, 0, 'f', 2);
}

QString UnifiedGridRenderer::getMsdfAtlasMemory() const {
    if (!m_msdfAtlas.isBuilt()) {
        return "MSDF atlas: N/A";
    }
    const QImage& image = m_msdfAtlas.image();
    if (image.isNull()) {
        return "MSDF atlas: N/A";
    }
    const double mb = static_cast<double>(image.sizeInBytes()) / (1024.0 * 1024.0);
    return QString("MSDF atlas: %1 MB").arg(mb, 0, 'f', 2);
}

double UnifiedGridRenderer::getUploadBandwidth() const {
    return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
    if (m_useGpuHeatmap && m_heatmapStream) {
        const auto snapshot = m_heatmapStream->snapshot();
        if (snapshot.gridWidth > 0) {
            return QString("%1/%2").arg(m_heatmapStream->writeColumn()).arg(snapshot.gridWidth);
        }
    }
    return "N/A";
}

int UnifiedGridRenderer::getDirtyRegionCount() const {
    if (m_useGpuHeatmap) {
        if (m_heatmapStream) {
            return m_heatmapStream->pendingUploadCount();
        }
    }
    return 0;
}

void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { 
    if (m_viewState && isVisible() && event->button() == Qt::LeftButton) { 
        m_viewState->handlePanStart(event->position()); 
        event->accept(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) { 
    if (m_viewState) { 
        m_viewState->handlePanMove(event->position()); 
        event->accept(); 
        update(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) {
    if (m_viewState) {
        const QPointF pan = m_viewState->getPanVisualOffset();
        const bool autoScroll = m_viewState->isAutoScrollEnabled();
        bool panAppliedToAuto = false;
        if (autoScroll && !pan.isNull() && width() > 0.0 && height() > 0.0 &&
            m_viewState->isTimeWindowValid() && m_autoScrollController) {
            panAppliedToAuto = m_autoScrollController->applyPanToAutoScroll(*m_viewState, width(), height());
            m_viewState->handlePanEnd(false);
            m_panSyncPending = true;
            update();
        } else {
            m_viewState->handlePanEnd(true);
        }
        event->accept();
        if (m_viewState->isAutoScrollEnabled() && m_heatmapStream && m_autoScrollController && !panAppliedToAuto) {
            m_autoScrollController->updateLagFromView(*m_viewState,
                                                      *m_heatmapStream,
                                                      m_timeAuthority.activeTimeframeMs());
        }
        if (autoScroll) {
            m_panSyncPending = true;
        }
        update();
    }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { 
    if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) { 
        m_viewState->handleZoomWithSensitivity(event->angleDelta().y(), event->position(), QSizeF(width(), height())); 
        update(); event->accept(); 
    } else event->ignore(); 
}

int UnifiedGridRenderer::getCurrentTimeResolution() const { return static_cast<int>(m_currentTimeframe_ms); }
double UnifiedGridRenderer::getCurrentPriceResolution() const { return m_dataProcessor ? m_dataProcessor->getPriceResolution() : 1.0; }
double UnifiedGridRenderer::getScreenWidth() const { return width(); }
double UnifiedGridRenderer::getScreenHeight() const { return height(); }
double UnifiedGridRenderer::getZoomFactor() const { return m_viewState ? m_viewState->getZoomFactor() : 1.0; }
qint64 UnifiedGridRenderer::getVisibleTimeStart() const { return m_viewState ? m_viewState->getVisibleTimeStart() : 0; }
qint64 UnifiedGridRenderer::getVisibleTimeEnd() const { return m_viewState ? m_viewState->getVisibleTimeEnd() : 0; }
double UnifiedGridRenderer::getMinPrice() const { return m_viewState ? m_viewState->getMinPrice() : 0.0; }
double UnifiedGridRenderer::getMaxPrice() const { return m_viewState ? m_viewState->getMaxPrice() : 0.0; }
QPointF UnifiedGridRenderer::getPanVisualOffset() const { return m_viewState ? m_viewState->getPanVisualOffset() : QPointF(0, 0); }

MappingFrameContext UnifiedGridRenderer::currentFrameContext() const {
    std::lock_guard<std::mutex> lock(m_frameContextMutex);
    return m_lastFrameContext;
}

TimeAxisMapping UnifiedGridRenderer::currentTimeAxisMapping() const {
    return currentFrameContext().mapping;
}

bool UnifiedGridRenderer::heatmapDataPriceRange(double& outMin, double& outMax) const {
    if (!m_heatmapStream) {
        return false;
    }
    const auto snapshot = m_heatmapStream->snapshot();
    if (snapshot.tickSize <= 0.0 || snapshot.maxPrice <= snapshot.minPrice) {
        return false;
    }
    outMin = snapshot.minPrice;
    outMax = snapshot.maxPrice;
    return true;
}

bool UnifiedGridRenderer::heatmapDataTimeRange(qint64& outStart, qint64& outEnd) const {
    if (!m_heatmapStream) {
        return false;
    }
    const auto snapshot = m_heatmapStream->snapshot();
    const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
        ? m_timeAuthority.activeTimeframeMs()
        : static_cast<int64_t>(snapshot.appendMs);
    if (cadenceMs <= 0 || snapshot.gridWidth <= 0) {
        return false;
    }
    const int64_t bufferSpanMs = static_cast<int64_t>(snapshot.gridWidth) * cadenceMs;
    if (bufferSpanMs <= 0) {
        return false;
    }
    int64_t dataEnd = 0;
    if (snapshot.lastSliceStartMs != std::numeric_limits<int64_t>::min()) {
        dataEnd = snapshot.lastSliceStartMs + cadenceMs;
    } else if (snapshot.timeOriginMs != 0) {
        dataEnd = snapshot.timeOriginMs + bufferSpanMs;
    } else {
        return false;
    }
    const int64_t dataStart = dataEnd - bufferSpanMs;
    if (dataEnd <= dataStart) {
        return false;
    }
    outStart = dataStart;
    outEnd = dataEnd;
    return true;
}

QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Size:%1x%2").arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
QString UnifiedGridRenderer::getViewportMathDebug() const {
    if (!m_viewState) {
        return "Viewport: N/A";
    }
    const auto snapshot = m_heatmapStream ? m_heatmapStream->snapshot() : HeatmapStreamState::Snapshot{};
    const QRectF bounds = boundingRect();
    const bool forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
    const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
        ? m_timeAuthority.activeTimeframeMs()
        : static_cast<int64_t>(snapshot.appendMs);

    UgrFrameMath::ViewportState viewportState;
    viewportState.valid = m_viewState->isTimeWindowValid();
    viewportState.timeStart = static_cast<double>(m_viewState->getVisibleTimeStart());
    viewportState.timeEnd = static_cast<double>(m_viewState->getVisibleTimeEnd());
    viewportState.minPrice = m_viewState->getMinPrice();
    viewportState.maxPrice = m_viewState->getMaxPrice();
    viewportState.panVisualOffset = m_viewState->getPanVisualOffset();
    viewportState.dragging = m_viewState->isDragging();
    viewportState = UgrFrameMath::applyDragPan(viewportState, bounds);

    UgrFrameMath::GridState gridState;
    gridState.gridWidth = gridWidth;
    gridState.gridHeight = gridHeight;
    gridState.cadenceMs = cadenceMs;
    gridState.timeOriginMs = snapshot.timeOriginMs;
    gridState.lastSliceStartMs = snapshot.lastSliceStartMs;
    gridState.filledColumns = snapshot.filledColumns;
    gridState.tickSize = snapshot.tickSize;
    gridState.dataMinPrice = snapshot.minPrice;
    gridState.dataMaxPrice = snapshot.maxPrice;
    gridState.forceFull = forceFull;

    const UgrFrameMath::RenderRects renderRects =
        UgrFrameMath::computeRenderRects(bounds, viewportState, gridState);
    const double viewTimeSpan = renderRects.viewTimeEnd - renderRects.viewTimeStart;
    const double viewPriceSpan = renderRects.viewMaxPrice - renderRects.viewMinPrice;

    QStringList lines;
    lines << "Viewport Math"
          << QString("view.time: %1 → %2 (%3 ms)")
                 .arg(static_cast<qint64>(renderRects.viewTimeStart))
                 .arg(static_cast<qint64>(renderRects.viewTimeEnd))
                 .arg(static_cast<qint64>(std::max(0.0, viewTimeSpan)))
          << QString("view.price: %1 → %2 (Δ%3)")
                 .arg(renderRects.viewMinPrice, 0, 'f', 4)
                 .arg(renderRects.viewMaxPrice, 0, 'f', 4)
                 .arg(std::max(0.0, viewPriceSpan), 0, 'f', 4)
          << QString("tick: %1  grid: %2x%3  append: %4")
                 .arg(snapshot.tickSize, 0, 'f', 6)
                 .arg(gridWidth)
                 .arg(gridHeight)
                 .arg(cadenceMs);

    if (cadenceMs > 0 && snapshot.tickSize > 0.0 &&
        snapshot.timeOriginMs != 0 && viewTimeSpan > 0.0 && viewPriceSpan > 0.0) {
        const double overlapStart = std::max(renderRects.viewTimeStart, renderRects.dataStart);
        const double overlapEnd = std::min(renderRects.viewTimeEnd, renderRects.dataEnd);
        const double overlapMin = std::max(renderRects.viewMinPrice, snapshot.minPrice);
        const double overlapMax = std::min(renderRects.viewMaxPrice, snapshot.maxPrice);

        lines << QString("data.time: %1 → %2").arg(static_cast<qint64>(renderRects.dataStart))
                                               .arg(static_cast<qint64>(renderRects.dataEnd))
              << QString("data.price: %1 → %2").arg(snapshot.minPrice, 0, 'f', 4)
                                                .arg(snapshot.maxPrice, 0, 'f', 4)
              << QString("overlap.time: %1 → %2")
                     .arg(static_cast<qint64>(overlapStart))
                     .arg(static_cast<qint64>(overlapEnd))
              << QString("overlap.price: %1 → %2")
                     .arg(overlapMin, 0, 'f', 4)
                     .arg(overlapMax, 0, 'f', 4);
        const double cellW = (renderRects.srcRect.width() > 0.0)
            ? (renderRects.drawRect.width() / renderRects.srcRect.width()) : 0.0;
        const double cellH = (renderRects.srcRect.height() > 0.0)
            ? (renderRects.drawRect.height() / renderRects.srcRect.height()) : 0.0;

        lines << QString("drawRect: x%1 y%2 w%3 h%4")
                     .arg(renderRects.drawRect.x(), 0, 'f', 1)
                     .arg(renderRects.drawRect.y(), 0, 'f', 1)
                     .arg(renderRects.drawRect.width(), 0, 'f', 1)
                     .arg(renderRects.drawRect.height(), 0, 'f', 1)
              << QString("srcRect: x%1 y%2 w%3 h%4")
                     .arg(renderRects.srcRect.x(), 0, 'f', 2)
                     .arg(renderRects.srcRect.y(), 0, 'f', 2)
                     .arg(renderRects.srcRect.width(), 0, 'f', 2)
                     .arg(renderRects.srcRect.height(), 0, 'f', 2)
              << QString("cell: %1 x %2 px").arg(cellW, 0, 'f', 2).arg(cellH, 0, 'f', 2)
              << QString("forceFull: %1  dragging: %2")
                     .arg(forceFull ? "yes" : "no")
                     .arg(m_viewState->isDragging() ? "yes" : "no");
    } else {
        lines << "data: N/A";
    }

    return lines.join('\n');
}

QString UnifiedGridRenderer::getDataPipelineDebug() const {
    QStringList lines;
    lines << "Data Pipeline";

    if (!m_heatmapStream) {
        lines << "stream: N/A";
        return lines.join('\n');
    }

    const auto snapshot = m_heatmapStream->snapshot();
    const int gridWidth = (snapshot.gridWidth > 0) ? snapshot.gridWidth : m_heatmapGridWidth;
    const int gridHeight = (snapshot.gridHeight > 0) ? snapshot.gridHeight : m_heatmapGridHeight;
    const int pendingUploads = m_heatmapStream->pendingUploadCount();
    const int writeColumn = m_heatmapStream->writeColumn();
    const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
        ? m_timeAuthority.activeTimeframeMs()
        : static_cast<int64_t>(snapshot.appendMs);
    const qint64 lastAppendMs = m_heatmapStream->lastAppendMs();
    const qint64 nowMs = m_heatmapClock.isValid() ? m_heatmapClock.elapsed() : 0;
    const qint64 ageMs = (lastAppendMs > 0 && nowMs >= lastAppendMs) ? (nowMs - lastAppendMs) : -1;

    lines << QString("grid: %1x%2  append: %3 ms")
                 .arg(gridWidth)
                 .arg(gridHeight)
                 .arg(cadenceMs)
          << QString("tick: %1  range: %2 → %3")
                 .arg(snapshot.tickSize, 0, 'f', 6)
                 .arg(snapshot.minPrice, 0, 'f', 4)
                 .arg(snapshot.maxPrice, 0, 'f', 4)
          << QString("last slice: %1  age: %2 ms")
                 .arg(snapshot.lastSliceStartMs)
                 .arg(ageMs)
          << QString("pending uploads: %1  ring cursor: %2/%3")
                 .arg(pendingUploads)
                 .arg(writeColumn)
                 .arg(gridWidth)
          << QString("liquidity labels: %1")
                 .arg(snapshot.liquidityAvailable ? "yes" : "no");

    if (snapshot.timeOriginMs != 0) {
        lines << QString("time origin: %1").arg(snapshot.timeOriginMs);
    }
    if (snapshot.streamBaseMs != std::numeric_limits<int64_t>::min()) {
        lines << QString("stream base: %1").arg(snapshot.streamBaseMs);
    }

    return lines.join('\n');
}
QString UnifiedGridRenderer::getPerformanceStats() const { return "N/A (SentinelMonitor removed)"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_currentFps.load(); }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
void UnifiedGridRenderer::togglePerformanceOverlay() { }

void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); update(); } }
