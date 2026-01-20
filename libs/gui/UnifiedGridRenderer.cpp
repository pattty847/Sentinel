/*
Sentinel — UnifiedGridRenderer
Role: Implements the data batching and rendering orchestration logic.
Inputs/Outputs: Buffers incoming data; passes it to rendering strategies in updatePaintNode.
Threading: Data receiving slots run on main thread; updatePaintNode runs on the render thread.
Performance: Batches high-frequency data events into single, throttled screen updates via a QTimer.
Integration: Defines the specific set of render strategies that compose the final chart.
Observability: Traces the data flow from reception to rendering via qDebug.
Related: UnifiedGridRenderer.h, HeatmapStrategy.h, TradeStrategy.h, CoordinateSystem.h.
Assumptions: The render strategies are compatible and can be layered together.
*/
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "datasources/IGridDataSource.hpp"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QSGSimpleTextureNode>
#include <QThread>
#include <QMetaObject>
#include <QMetaType>
#include <QTimer>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QtEndian>
#include <cmath>
// #include <algorithm>
#include <algorithm>
// #include <cmath>

// New modular architecture includes
#include "render/GridViewState.hpp"
#include "render/DataProcessor.hpp"
#include "render/HeatmapIntensityNode.hpp"

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    setAcceptHoverEvents(false);  // Reduce event capture
    
    init();
    sLog_App("UnifiedGridRenderer V2: Initialized successfully");
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
    sLog_App("UnifiedGridRenderer destructor - cleaning up...");
    
    if (m_dataProcessor) {
        if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
            // CRITICAL: stopProcessing() must be called on the worker thread where the timer lives
            // Use BlockingQueuedConnection to ensure it completes before we destroy the thread
            QMetaObject::invokeMethod(m_dataProcessor.get(), &DataProcessor::stopProcessing, Qt::BlockingQueuedConnection);
        } else {
            // Thread not running - safe to call directly (timer already stopped or object on main thread)
            m_dataProcessor->stopProcessing();
        }
        disconnect(m_dataProcessor.get(), nullptr, this, nullptr);
    }
    
    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
        m_dataProcessorThread->quit();
        
        // Wait for thread to finish and process all pending events
        if (!m_dataProcessorThread->wait(5000)) {
            sLog_App("Thread did not finish in time, terminating...");
            m_dataProcessorThread->terminate();
            m_dataProcessorThread->wait(1000);
        }
    }

    m_dataProcessor.reset();
    m_dataProcessorThread.reset();
    
    sLog_App("UnifiedGridRenderer cleanup complete");
}

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    Q_UNUSED(trade);
}

// onLiveOrderBookUpdated(QString) removed: legacy pass-through; MainWindow connects Core→DataProcessor directly

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
    // Mark transform dirty for rendering update
    update();
}


void UnifiedGridRenderer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    
    if (newGeometry.size() != oldGeometry.size()) {
        sLog_Render("UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        // Keep GridViewState in sync with the item size for accurate coord math
        if (m_viewState) {
            m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
        }

        // Size change only affects transform, not geometry topology
        update();
    }
}

void UnifiedGridRenderer::componentComplete() {
    // Called by Qt upon component initialization
    QQuickItem::componentComplete();
    
    // Set viewport size immediately when component is ready
    if (m_viewState && width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
        sLog_App("Component complete: Set initial viewport size to " << width() << "x" << height() << " pixels");
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
        m_autoScrollSpanMs = 0;
        emit autoScrollPaddingFracChanged();
    }
}

void UnifiedGridRenderer::setAutoScrollSmoothEnabled(bool enabled) {
    if (m_smoothAutoScrollEnabled != enabled) {
        m_smoothAutoScrollEnabled = enabled;
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
    // Delegate to DataProcessor
    if (m_dataProcessor) {
        m_dataProcessor->clearData();
    }
    update();
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_dataProcessor && resolution > 0) {
        m_dataProcessor->setPriceResolution(resolution);
        update();
    }
}

void UnifiedGridRenderer::setGridMode(int mode) {
    double priceRes[] = {2.5, 5.0, 10.0};
    int timeRes[] = {50, 100, 250};
    if (mode >= 0 && mode <= 2) {
        setPriceResolution(priceRes[mode]);
        setTimeframe(timeRes[mode]);
    }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
    if (m_currentTimeframe_ms != timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        if (m_useGpuHeatmap && timeframe_ms > 0) {
            m_heatmapAppendMs = timeframe_ms;
        }
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        if (m_dataProcessor) m_dataProcessor->addTimeframe(timeframe_ms);
        update();
        emit timeframeChanged();
    }
}

void UnifiedGridRenderer::setLiquidityLabelMode(int mode) {
    if (m_liquidityLabelMode == mode) {
        return;
    }
    m_liquidityLabelMode = mode;
    m_heatmapLabelDirty = true;
    update();
    emit liquidityLabelModeChanged();
}

void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); update(); } }

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        update();
        emit autoScrollEnabledChanged();
        sLog_Render("Auto-scroll: "<< (enabled ? "ENABLED" : "DISABLED"));
        if (enabled && m_viewState->isTimeWindowValid() && m_heatmapAppendMs > 0 &&
            m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min()) {
            const int64_t spanMs = std::max<int64_t>(1, m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
            const int64_t padMs = static_cast<int64_t>(spanMs * m_autoScrollPaddingFrac);
            m_autoScrollLagMs = (m_viewState->getVisibleTimeEnd() - (m_heatmapLastSliceStartMs + m_heatmapAppendMs)) - padMs;
            const int64_t maxSpanMs = static_cast<int64_t>(m_heatmapGridSize) * m_heatmapAppendMs;
            m_autoScrollSpanMs = std::min(spanMs, maxSpanMs);
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
    
    // NO pan offset applied - that's handled by the node transform in updatePaintNode
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
    
    // NO pan offset removed - node transform handles it
    return CoordinateSystem::screenToWorld(QPointF(screenX, screenY), viewport);
}

void UnifiedGridRenderer::init() {
    m_useGpuHeatmap = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP");
    if (m_useGpuHeatmap) {
        bool ok = false;
        const int envSize = qgetenv("SENTINEL_HEATMAP_GRID").toInt(&ok);
        if (ok && envSize > 0) {
            m_heatmapGridSize = envSize;
        }
        ok = false;
        const double recenter = qgetenv("SENTINEL_HEATMAP_RECENTER").toDouble(&ok);
        if (ok && recenter > 0.0) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), [this, recenter]() {
                m_dataProcessor->setHeatmapRecenterFraction(recenter);
            }, Qt::QueuedConnection);
        }
        m_heatmapClock.start();
    }

    // Register metatypes for cross-thread signal/slot connections
    qRegisterMetaType<Trade>("Trade");
    
    m_viewState = std::make_unique<GridViewState>(this);
    
    // Create DataProcessor on worker thread for background processing
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessor = std::make_unique<DataProcessor>();  // No parent - will be moved to thread
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    
    // Connect signals with QueuedConnection for thread safety
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
                   double liquidityScale) {
                Q_UNUSED(sliceEndMs);
                const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapTextureDirty = true;
                    m_heatmapClock.start();
                }
                if (column.isEmpty()) {
                    if (debug) {
                        sLog_Render("GPU HEATMAP DROP: enabled=" << m_useGpuHeatmap
                                    << " grid=" << m_heatmapGridSize
                                    << " bytes=" << column.size());
                    }
                    return;
                }
                if (column.size() > 0 && column.size() != m_heatmapGridSize) {
                    m_heatmapGridSize = column.size();
                    m_heatmapTextureDirty = true;
                    m_heatmapWriteColumn = 0;
                    m_heatmapTimeOriginMs = 0;
                    m_heatmapLastSliceStartMs = std::numeric_limits<int64_t>::min();
                    m_heatmapHaveLastColumn = false;
                    m_heatmapLastColumnData.clear();
                    m_heatmapHaveLastLiquidity = false;
                    m_heatmapLastLiquidityColumn.clear();
                    m_heatmapLastLiquidityScale = 1.0;
                    m_heatmapLiquidityAvailable = false;
                    {
                        std::lock_guard<std::mutex> lock(m_heatmapLiquidityMutex);
                        m_heatmapLiquidityRing.assign(static_cast<size_t>(m_heatmapGridSize) * m_heatmapGridSize, 0);
                        m_heatmapLiquidityScales.assign(m_heatmapGridSize, 1.0);
                    }
                    m_heatmapLabelDirty = true;
                    m_heatmapViewportInitialized = false;
                    {
                        std::lock_guard<std::mutex> lock(m_heatmapUploadMutex);
                        m_heatmapPendingColumns.clear();
                    }
                }

                if (debug) {
                    static int debugCount = 0;
                    ++debugCount;
                    if (debugCount <= 5 || debugCount % 50 == 0) {
                        int bidCount = 0;
                        int askCount = 0;
                        unsigned char minByte = 255;
                        unsigned char maxByte = 0;
                        const auto* bytes = reinterpret_cast<const unsigned char*>(column.constData());
                        for (int i = 0; i < column.size(); ++i) {
                            const unsigned char v = bytes[i];
                            if (v == 0) {
                                continue;
                            }
                            minByte = std::min(minByte, v);
                            maxByte = std::max(maxByte, v);
                            if (v >= 128) {
                                ++askCount;
                            } else {
                                ++bidCount;
                            }
                        }
                        sLog_Render("GPU HEATMAP BYTES: bids=" << bidCount
                                    << " asks=" << askCount
                                    << " min=" << static_cast<int>(minByte)
                                    << " max=" << static_cast<int>(maxByte));
                    }
                }

                m_heatmapMinPrice = minPrice;
                m_heatmapMaxPrice = maxPrice;
                m_heatmapTickSize = tickSize;
                if (timeframeMs > 0) {
                    m_heatmapAppendMs = static_cast<int>(timeframeMs);
                }

                if (m_heatmapTimeOriginMs == 0) {
                    m_heatmapTimeOriginMs = sliceStartMs;
                }
                if (m_heatmapStreamBaseMs == std::numeric_limits<int64_t>::min()) {
                    m_heatmapStreamBaseMs = sliceStartMs - m_heatmapClock.elapsed();
                } else {
                    // Keep a continuous base; on each slice, re-anchor to reduce drift.
                    m_heatmapStreamBaseMs = sliceStartMs - m_heatmapClock.elapsed();
                }

                int step = 1;
                if (m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min() &&
                    m_heatmapAppendMs > 0) {
                    const int64_t dt = sliceStartMs - m_heatmapLastSliceStartMs;
                    if (dt > 0) {
                        const int64_t rawStep = dt / m_heatmapAppendMs;
                        step = static_cast<int>(std::max<int64_t>(1, rawStep));
                    }
                }

                const int expectedLiquidityBytes = m_heatmapGridSize * static_cast<int>(sizeof(uint16_t));
                const bool haveLiquidityColumn = (liquidityColumn.size() == expectedLiquidityBytes);
                QByteArray fillColumn;
                if (!m_heatmapHaveLastColumn) {
                    fillColumn = QByteArray(column.size(), 0);
                } else {
                    fillColumn = m_heatmapLastColumnData;
                }

                QByteArray fillLiquidityColumn;
                double fillLiquidityScale = m_heatmapLastLiquidityScale;
                if (!m_heatmapHaveLastLiquidity || expectedLiquidityBytes <= 0) {
                    fillLiquidityColumn = QByteArray(expectedLiquidityBytes, 0);
                    fillLiquidityScale = 1.0;
                } else {
                    fillLiquidityColumn = m_heatmapLastLiquidityColumn;
                }

                {
                    std::lock_guard<std::mutex> lock(m_heatmapUploadMutex);
                    for (int i = 0; i < step - 1; ++i) {
                        m_heatmapWriteColumn = (m_heatmapWriteColumn + 1) % m_heatmapGridSize;
                        m_heatmapPendingColumns.push_back({m_heatmapWriteColumn, fillColumn});
                    }

                    m_heatmapWriteColumn = (m_heatmapWriteColumn + 1) % m_heatmapGridSize;
                    m_heatmapPendingColumns.push_back({m_heatmapWriteColumn, column});
                }

                if (expectedLiquidityBytes > 0) {
                    std::lock_guard<std::mutex> lock(m_heatmapLiquidityMutex);
                    if (m_heatmapLiquidityRing.size() != static_cast<size_t>(m_heatmapGridSize) * m_heatmapGridSize) {
                        m_heatmapLiquidityRing.assign(static_cast<size_t>(m_heatmapGridSize) * m_heatmapGridSize, 0);
                        m_heatmapLiquidityScales.assign(m_heatmapGridSize, 1.0);
                    }
                    for (int i = 0; i < step - 1; ++i) {
                        const int columnIndex = (m_heatmapWriteColumn - (step - 1 - i) + m_heatmapGridSize) % m_heatmapGridSize;
                        if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                            const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                            for (int y = 0; y < m_heatmapGridSize; ++y) {
                                const uint16_t raw = qFromLittleEndian(src[y]);
                                m_heatmapLiquidityRing[static_cast<size_t>(y) * m_heatmapGridSize + columnIndex] = raw;
                            }
                            m_heatmapLiquidityScales[columnIndex] = fillLiquidityScale;
                        }
                    }
                    if (haveLiquidityColumn) {
                        const auto* src = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
                        for (int y = 0; y < m_heatmapGridSize; ++y) {
                            const uint16_t raw = qFromLittleEndian(src[y]);
                            m_heatmapLiquidityRing[static_cast<size_t>(y) * m_heatmapGridSize + m_heatmapWriteColumn] = raw;
                        }
                        m_heatmapLiquidityScales[m_heatmapWriteColumn] = (liquidityScale > 0.0) ? liquidityScale : 1.0;
                        m_heatmapLastLiquidityColumn = liquidityColumn;
                        m_heatmapLastLiquidityScale = (liquidityScale > 0.0) ? liquidityScale : 1.0;
                        m_heatmapHaveLastLiquidity = true;
                        m_heatmapLiquidityAvailable = true;
                    } else {
                        if (fillLiquidityColumn.size() == expectedLiquidityBytes) {
                            const auto* src = reinterpret_cast<const uint16_t*>(fillLiquidityColumn.constData());
                            for (int y = 0; y < m_heatmapGridSize; ++y) {
                                const uint16_t raw = qFromLittleEndian(src[y]);
                                m_heatmapLiquidityRing[static_cast<size_t>(y) * m_heatmapGridSize + m_heatmapWriteColumn] = raw;
                            }
                            m_heatmapLiquidityScales[m_heatmapWriteColumn] = fillLiquidityScale;
                        }
                        m_heatmapLiquidityAvailable = false;
                    }
                }
                m_heatmapLastSliceStartMs = sliceStartMs;
                m_heatmapLastColumnData = column;
                m_heatmapHaveLastColumn = true;
                m_heatmapLabelDirty = true;

                m_heatmapLastAppendMs = m_heatmapClock.elapsed();
                if (!m_heatmapTimeOffsetFrozen && m_heatmapGridSize > 0) {
                    const int oldestColumn = (m_heatmapWriteColumn + 1) % m_heatmapGridSize;
                    const float offset = static_cast<float>(oldestColumn) /
                                         static_cast<float>(m_heatmapGridSize);
                    m_heatmapTimeOffset.store(offset);
                }
                if (debug) {
                    sLog_Render("GPU HEATMAP ENQUEUE: col=" << m_heatmapWriteColumn
                                << " step=" << step
                                << " tf=" << timeframeMs
                                << " range=$" << m_heatmapMinPrice << "-$" << m_heatmapMaxPrice);
                }
                if (!m_heatmapViewportInitialized && m_viewState && timeframeMs > 0 && m_heatmapGridSize > 0) {
                    const int64_t maxSpanMs = std::max<int64_t>(1, static_cast<int64_t>(m_heatmapGridSize - 1) * timeframeMs);
                    if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
                        m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * (1.0 - m_autoScrollPaddingFrac));
                        if (m_autoScrollSpanMs <= 0) {
                            m_autoScrollSpanMs = maxSpanMs;
                        }
                    }
                    const int64_t spanMs = m_autoScrollSpanMs;
                    const int64_t padMs = static_cast<int64_t>(spanMs * m_autoScrollPaddingFrac);
                    const int64_t viewEnd = sliceStartMs + timeframeMs - padMs;
                    const int64_t viewStart = viewEnd - spanMs;
                    if (viewEnd > viewStart) {
                        m_viewState->setViewport(viewStart, viewEnd, m_heatmapMinPrice, m_heatmapMaxPrice);
                        m_heatmapViewportInitialized = true;
                        if (debug) {
                            sLog_Render("GPU HEATMAP VIEWPORT INIT: [" << viewStart << "-" << viewEnd
                                        << "] $" << m_heatmapMinPrice << "-$" << m_heatmapMaxPrice);
                        }
                    }
                }

                if (m_viewState && m_viewState->isAutoScrollEnabled() && timeframeMs > 0 && !m_viewState->isDragging()) {
                    const int64_t maxSpanMs = static_cast<int64_t>(m_heatmapGridSize) * timeframeMs;
                    if (!m_smoothAutoScrollEnabled) {
                        if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
                            m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * (1.0 - m_autoScrollPaddingFrac));
                            if (m_autoScrollSpanMs <= 0) {
                                m_autoScrollSpanMs = maxSpanMs;
                            }
                        }
                        const int64_t clampedSpanMs = std::min(m_autoScrollSpanMs, maxSpanMs);
                        const int64_t padMs = static_cast<int64_t>(clampedSpanMs * m_autoScrollPaddingFrac);
                        const int64_t viewEnd = sliceStartMs + timeframeMs + m_autoScrollLagMs - padMs;
                        const int64_t viewStart = viewEnd - clampedSpanMs;

                        const double priceSpan = std::max(1e-6, m_viewState->getMaxPrice() - m_viewState->getMinPrice());
                        const double heatmapMid = (m_heatmapMinPrice + m_heatmapMaxPrice) * 0.5;
                        double minPrice = m_viewState->getMinPrice();
                        double maxPrice = m_viewState->getMaxPrice();
                        if (maxPrice < m_heatmapMinPrice || minPrice > m_heatmapMaxPrice) {
                            minPrice = heatmapMid - priceSpan * 0.5;
                            maxPrice = heatmapMid + priceSpan * 0.5;
                        }

                        m_viewState->setViewport(viewStart, viewEnd, minPrice, maxPrice);
                        if (m_panSyncPending) {
                            m_viewState->clearPanVisualOffset();
                            m_panSyncPending = false;
                        }
                    }
                }
                update();
            },
            Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::heatmapRangeReset,
            this,
            [this](double minPrice, double maxPrice, double tickSize, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    m_useGpuHeatmap = true;
                    m_heatmapTextureDirty = true;
                    m_heatmapClock.start();
                }
                m_heatmapMinPrice = minPrice;
                m_heatmapMaxPrice = maxPrice;
                m_heatmapTickSize = tickSize;
                if (gridHeight > 0) {
                    m_heatmapGridSize = gridHeight;
                }
                m_heatmapTextureDirty = true;
                m_heatmapWriteColumn = 0;
                m_heatmapTimeOriginMs = 0;
                m_heatmapLastSliceStartMs = std::numeric_limits<int64_t>::min();
                m_heatmapHaveLastColumn = false;
                m_heatmapLastColumnData.clear();
                m_heatmapHaveLastLiquidity = false;
                m_heatmapLastLiquidityColumn.clear();
                m_heatmapLastLiquidityScale = 1.0;
                m_heatmapLiquidityAvailable = false;
                {
                    std::lock_guard<std::mutex> lock(m_heatmapLiquidityMutex);
                    m_heatmapLiquidityRing.assign(static_cast<size_t>(m_heatmapGridSize) * m_heatmapGridSize, 0);
                    m_heatmapLiquidityScales.assign(m_heatmapGridSize, 1.0);
                }
                m_heatmapLabelDirty = true;
                m_autoScrollSpanMs = 0;
                if (m_viewState && m_viewState->isAutoScrollEnabled()) {
                    m_heatmapViewportInitialized = false;
                }
                {
                    std::lock_guard<std::mutex> lock(m_heatmapUploadMutex);
                    m_heatmapPendingColumns.clear();
                }
                update();
            },
            Qt::QueuedConnection);
    
    // Start the worker thread
    m_dataProcessorThread->start();
    
    // Initialize viewport size immediately to avoid 0x0 transforms
    if (width() > 0 && height() > 0) {
        m_viewState->setViewportSize(width(), height());
    }

    // Dependencies will be set later when setDataCache() is called
    // Set ViewState immediately as it's available
    QMetaObject::invokeMethod(m_dataProcessor.get(), [this]() {
        m_dataProcessor->setGridViewState(m_viewState.get());
    }, Qt::QueuedConnection);
    
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
            m_dataProcessor->setHeatmapGridHeight(m_heatmapGridSize);
            m_dataProcessor->setHeatmapIntensityScale(m_intensityScale);
        }, Qt::QueuedConnection);

        m_heatmapRenderTimer = new QTimer(this);
        connect(m_heatmapRenderTimer, &QTimer::timeout, this, [this]() {
            if (!m_useGpuHeatmap || m_heatmapGridSize <= 0 || m_heatmapAppendMs <= 0) {
                return;
            }
            const bool dragging = (m_viewState && m_viewState->isDragging());
            if (dragging) {
                if (!m_heatmapTimeOffsetFrozen) {
                    m_heatmapTimeOffsetFrozen = true;
                    m_heatmapFrozenTimeOffset = m_heatmapTimeOffset.load();
                }
                update();
                return;
            }
            if (m_heatmapTimeOffsetFrozen) {
                m_heatmapTimeOffsetFrozen = false;
            }
            const qint64 nowMs = m_heatmapClock.elapsed();
            const qint64 delta = nowMs - m_heatmapLastAppendMs;
            const bool useFractionalOffset = (m_viewState && m_viewState->isAutoScrollEnabled() && !m_smoothAutoScrollEnabled);
            const float frac = useFractionalOffset
                ? std::clamp(static_cast<float>(delta) / static_cast<float>(m_heatmapAppendMs), 0.0f, 1.0f)
                : 0.0f;
            const int oldestColumn = (m_heatmapWriteColumn + 1) % m_heatmapGridSize;
            const float offset = (static_cast<float>(oldestColumn) + frac) /
                                 static_cast<float>(m_heatmapGridSize);
            m_heatmapTimeOffset.store(offset);
            if (m_smoothAutoScrollEnabled &&
                m_viewState && m_viewState->isAutoScrollEnabled() && !m_viewState->isDragging() &&
                m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min()) {
                const int64_t maxSpanMs = static_cast<int64_t>(m_heatmapGridSize) * m_heatmapAppendMs;
                if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
                    m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * (1.0 - m_autoScrollPaddingFrac));
                    if (m_autoScrollSpanMs <= 0) {
                        m_autoScrollSpanMs = maxSpanMs;
                    }
                }
                const int64_t clampedSpanMs = std::min(m_autoScrollSpanMs, maxSpanMs);
                const int64_t padMs = static_cast<int64_t>(clampedSpanMs * m_autoScrollPaddingFrac);
                const int64_t streamNowMs = (m_heatmapStreamBaseMs != std::numeric_limits<int64_t>::min())
                    ? (m_heatmapStreamBaseMs + nowMs + m_heatmapAppendMs)
                    : (m_heatmapLastSliceStartMs + m_heatmapAppendMs);
                const int64_t viewEnd = streamNowMs + m_autoScrollLagMs - padMs;
                const int64_t viewStart = viewEnd - clampedSpanMs;

                const double priceSpan = std::max(1e-6, m_viewState->getMaxPrice() - m_viewState->getMinPrice());
                const double heatmapMid = (m_heatmapMinPrice + m_heatmapMaxPrice) * 0.5;
                double minPrice = m_viewState->getMinPrice();
                double maxPrice = m_viewState->getMaxPrice();
                if (maxPrice < m_heatmapMinPrice || minPrice > m_heatmapMaxPrice) {
                    minPrice = heatmapMid - priceSpan * 0.5;
                    maxPrice = heatmapMid + priceSpan * 0.5;
                }

                m_viewState->setViewport(viewStart, viewEnd, minPrice, maxPrice);
                if (m_panSyncPending) {
                    m_viewState->clearPanVisualOffset();
                    m_panSyncPending = false;
                }
            }
            update();
        });
        m_heatmapRenderTimer->start(16);
    }
}

// Dense data access - set cache on both UGR and DataProcessor
void UnifiedGridRenderer::setDataSource(IGridDataSource* source) {
    m_dataSource = source;
    
    // Also set the cache on DataProcessor using thread-safe invocation
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this, source]() {
            m_dataProcessor->setDataSource(source);
        }, Qt::QueuedConnection);
    }
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0) { 
        return oldNode;
    }

    if (m_useGpuHeatmap) {
        auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
        if (!texNode) {
            texNode = new HeatmapIntensityNode();
            m_heatmapTextureDirty = true;
        }

        if (m_heatmapTextureDirty) {
            ensureHeatmapImage();
            ensureHeatmapPaletteImage();
            auto* intensityTexture = window()->createTextureFromImage(m_heatmapImage);
            if (!intensityTexture) {
                QImage fallback = m_heatmapImage.convertToFormat(QImage::Format_Grayscale8);
                intensityTexture = window()->createTextureFromImage(fallback);
            }
            auto* paletteTexture = window()->createTextureFromImage(m_heatmapPaletteImage);
            if (intensityTexture && paletteTexture) {
                intensityTexture->setFiltering(QSGTexture::Nearest);
                paletteTexture->setFiltering(QSGTexture::Linear);
                texNode->setTextures(intensityTexture, paletteTexture);
                texNode->setGamma(1.05f);
                texNode->setContrast(1.15f);
                m_heatmapTextureDirty = false;
            }
        }

        const QRectF bounds = boundingRect();
        QRectF drawRect = bounds;
        texNode->setRect(drawRect);

        if (m_viewState && m_viewState->isTimeWindowValid() &&
            m_heatmapAppendMs > 0 && m_heatmapTickSize > 0.0 && m_heatmapTimeOriginMs != 0) {
            const qint64 timeStart = m_viewState->getVisibleTimeStart();
            const qint64 timeEnd = m_viewState->getVisibleTimeEnd();
            const double minPrice = m_viewState->getMinPrice();
            const double maxPrice = m_viewState->getMaxPrice();

            double timeStartF = static_cast<double>(timeStart);
            double timeEndF = static_cast<double>(timeEnd);
            double minPriceF = minPrice;
            double maxPriceF = maxPrice;

            const QPointF pan = m_viewState->getPanVisualOffset();
            const double timeRange = static_cast<double>(timeEnd - timeStart);
            const double priceRange = maxPrice - minPrice;
            if (!pan.isNull() && bounds.width() > 0.0 && bounds.height() > 0.0 &&
                timeRange > 0.0 && priceRange > 0.0 && m_viewState->isDragging()) {
                const double timePixelsToUnits = timeRange / bounds.width();
                const double pricePixelsToUnits = priceRange / bounds.height();
                const double timeDelta = -pan.x() * timePixelsToUnits;
                const double priceDelta = pan.y() * pricePixelsToUnits;
                timeStartF += timeDelta;
                timeEndF += timeDelta;
                minPriceF += priceDelta;
                maxPriceF += priceDelta;
            }

            if (qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL")) {
                drawRect = bounds;
                texNode->setRect(drawRect);
                texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
                texNode->setTimeOffset(0.0f);
            } else if (timeEndF > timeStartF && maxPriceF > minPriceF) {
                const double maxCoord = static_cast<double>(m_heatmapGridSize);
                const double viewTimeSpan = timeEndF - timeStartF;
                const double viewPriceSpan = maxPriceF - minPriceF;
                if (viewTimeSpan <= 0.0 || viewPriceSpan <= 0.0) {
                    drawRect = QRectF();
                    texNode->setRect(drawRect);
                    texNode->setSourceRect(QRectF());
                } else {
                    const int64_t lastSlice = m_heatmapLastSliceStartMs;
                    const int64_t bufferSpanMs = static_cast<int64_t>(m_heatmapGridSize) * m_heatmapAppendMs;
                    double dataEnd = (lastSlice != std::numeric_limits<int64_t>::min() && bufferSpanMs > 0)
                        ? static_cast<double>(lastSlice + m_heatmapAppendMs)
                        : static_cast<double>(m_heatmapTimeOriginMs + bufferSpanMs);
                    double dataStart = dataEnd - static_cast<double>(bufferSpanMs);
                    const bool dragging = m_viewState && m_viewState->isDragging();
                    if (dragging) {
                        if (!m_heatmapDragFrozen) {
                            m_heatmapDragFrozen = true;
                            m_heatmapDragDataStart = dataStart;
                            m_heatmapDragDataEnd = dataEnd;
                        }
                        dataStart = m_heatmapDragDataStart;
                        dataEnd = m_heatmapDragDataEnd;
                    } else if (m_heatmapDragFrozen) {
                        m_heatmapDragFrozen = false;
                    }

                    const double dataMin = m_heatmapMinPrice;
                    const double dataMax = m_heatmapMaxPrice;

                    const double overlapStart = std::max(timeStartF, dataStart);
                    const double overlapEnd = std::min(timeEndF, dataEnd);
                    const double overlapMin = std::max(minPriceF, dataMin);
                    const double overlapMax = std::min(maxPriceF, dataMax);

                    if (overlapEnd <= overlapStart || overlapMax <= overlapMin) {
                        drawRect = QRectF();
                        texNode->setRect(drawRect);
                        texNode->setSourceRect(QRectF());
                    } else {
                        const double overlapTimeSpan = overlapEnd - overlapStart;
                        const double overlapPriceSpan = overlapMax - overlapMin;

                        const double timeRatioStart = (overlapStart - timeStartF) / viewTimeSpan;
                        const double timeRatioEnd = (overlapEnd - timeStartF) / viewTimeSpan;
                        const double priceRatioTop = (maxPriceF - overlapMax) / viewPriceSpan;
                        const double priceRatioBottom = (maxPriceF - overlapMin) / viewPriceSpan;

                        drawRect = QRectF(
                            bounds.x() + bounds.width() * timeRatioStart,
                            bounds.y() + bounds.height() * priceRatioTop,
                            bounds.width() * (timeRatioEnd - timeRatioStart),
                            bounds.height() * (priceRatioBottom - priceRatioTop));
                        texNode->setRect(drawRect);

                        const double srcW = std::clamp(overlapTimeSpan / m_heatmapAppendMs, 1.0, maxCoord);
                        const double srcH = std::clamp(overlapPriceSpan / m_heatmapTickSize, 1.0, maxCoord);
                        double srcX = (overlapStart - dataStart) / m_heatmapAppendMs;
                        double srcY = (m_heatmapMaxPrice - overlapMax) / m_heatmapTickSize;
                        srcX = std::clamp(srcX, 0.0, maxCoord - srcW);
                        srcY = std::clamp(srcY, 0.0, maxCoord - srcH);
                        texNode->setSourceRect(QRectF(srcX, srcY, srcW, srcH));
                    }
                }
            } else {
                drawRect = bounds;
                texNode->setRect(drawRect);
                texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
            }
        } else {
            drawRect = bounds;
            texNode->setRect(drawRect);
            texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
        }

        const bool forceFull = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL");
        if (!forceFull) {
            texNode->setTimeOffset(m_heatmapTimeOffset.load());
        }

        if (!drawRect.isEmpty() && !bounds.isEmpty()) {
            const QRectF srcRect = texNode->getSourceRect();
            const QSize labelSize = drawRect.size().toSize();
            if (!labelSize.isEmpty() && srcRect.width() > 0.0 && srcRect.height() > 0.0 && m_heatmapLiquidityAvailable) {
                const float cellW = static_cast<float>(drawRect.width()) / std::max(1.0, srcRect.width());
                const float cellH = static_cast<float>(drawRect.height()) / std::max(1.0, srcRect.height());
                const float minCell = std::min(cellW, cellH);
                const float labelThreshold = 18.0f;
                if (minCell >= labelThreshold) {
                    const int gridSize = m_heatmapGridSize;
                    const float timeOffset = forceFull ? 0.0f : m_heatmapTimeOffset.load();
                    const float baseX = static_cast<float>(srcRect.x()) + (timeOffset * gridSize);
                    const int startX = static_cast<int>(std::floor(baseX));
                    const float fracX = baseX - static_cast<float>(startX);

                    const float baseY = static_cast<float>(srcRect.y());
                    const int startY = static_cast<int>(std::floor(baseY));
                    const float fracY = baseY - static_cast<float>(startY);

                    const float rawFontPx = std::clamp(minCell * 0.45f, 8.0f, 28.0f);
                    const int fontBucket = quantizeFontPx(rawFontPx);
                    const uint64_t viewportVersion = m_viewState ? m_viewState->getViewportVersion() : 0;

                    if (labelSize != m_heatmapLabelPixelSize ||
                        srcRect.size() != m_heatmapLabelSourceRect.size() ||
                        startX != m_heatmapLabelStartX ||
                        startY != m_heatmapLabelStartY ||
                        fontBucket != m_heatmapLabelFontBucket ||
                        viewportVersion != m_heatmapLabelViewportVersion) {
                        m_heatmapLabelPixelSize = labelSize;
                        m_heatmapLabelSourceRect = srcRect;
                        m_heatmapLabelStartX = startX;
                        m_heatmapLabelStartY = startY;
                        m_heatmapLabelFontBucket = fontBucket;
                        m_heatmapLabelViewportVersion = viewportVersion;
                        m_heatmapLabelDirty = true;
                    }

                    auto* labelNode = dynamic_cast<QSGSimpleTextureNode*>(texNode->firstChild());
                    if (window()) {
                        if (!labelNode) {
                            labelNode = new QSGSimpleTextureNode();
                            texNode->appendChildNode(labelNode);
                        }

                        if (m_heatmapLabelDirty) {
                            HeatmapLabelRequest request;
                            request.srcRect = srcRect;
                            request.labelSize = labelSize;
                            request.startX = startX;
                            request.startY = startY;
                            request.cellW = cellW;
                            request.cellH = cellH;
                            request.fontPx = fontBucket;
                            request.fontBucket = fontBucket;
                            request.viewportVersion = viewportVersion;
                            request.valid = true;
                            {
                                std::lock_guard<std::mutex> lock(m_heatmapLabelRequestMutex);
                                m_heatmapLabelRequest = request;
                            }
                            QMetaObject::invokeMethod(this, [this]() { buildHeatmapLabelImage(); }, Qt::QueuedConnection);
                            m_heatmapLabelDirty = false;
                        }

                        const int labelVersion = m_heatmapLabelVersion.load();
                        if (labelVersion != m_heatmapLabelTextureVersion) {
                            QImage labelImage;
                            {
                                std::lock_guard<std::mutex> lock(m_heatmapLabelMutex);
                                labelImage = m_heatmapLabelImage;
                                m_heatmapLabelActiveStartX = m_heatmapLabelBuiltStartX;
                                m_heatmapLabelActiveStartY = m_heatmapLabelBuiltStartY;
                            }
                            if (!labelImage.isNull()) {
                                auto* labelTex = window()->createTextureFromImage(labelImage);
                                labelTex->setFiltering(QSGTexture::Nearest);
                                labelNode->setTexture(labelTex);
                                labelNode->setOwnsTexture(true);
                                m_heatmapLabelTextureVersion = labelVersion;
                            }
                        }

                        if (labelNode && labelNode->texture()) {
                            const QSize texSize = labelNode->texture()->textureSize();
                            const float deltaX = baseX - static_cast<float>(m_heatmapLabelActiveStartX);
                            const float deltaY = baseY - static_cast<float>(m_heatmapLabelActiveStartY);
                            const float shiftPx = deltaX * cellW;
                            const float shiftPy = deltaY * cellH;
                            labelNode->setRect(QRectF(drawRect.x() - shiftPx,
                                                      drawRect.y() - shiftPy,
                                                      texSize.width(),
                                                      texSize.height()));
                        }
                    }
                } else {
                    if (auto* labelNode = dynamic_cast<QSGSimpleTextureNode*>(texNode->firstChild())) {
                        labelNode->setRect(QRectF());
                        m_heatmapLabelDirty = true;
                    }
                }
            } else {
                if (auto* labelNode = dynamic_cast<QSGSimpleTextureNode*>(texNode->firstChild())) {
                    labelNode->setRect(QRectF());
                }
            }
        } else {
            if (auto* labelNode = dynamic_cast<QSGSimpleTextureNode*>(texNode->firstChild())) {
                labelNode->setRect(QRectF());
            }
        }

        std::vector<HeatmapPendingColumn> pendingUploads;
        {
            std::lock_guard<std::mutex> lock(m_heatmapUploadMutex);
            if (!m_heatmapPendingColumns.empty()) {
                pendingUploads.swap(m_heatmapPendingColumns);
            }
        }
        for (auto& upload : pendingUploads) {
            texNode->enqueueColumn(upload.x, std::move(upload.data));
        }

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

        return texNode;
    }

    return oldNode;
}

void UnifiedGridRenderer::ensureHeatmapImage() {
    if (!m_heatmapImage.isNull() && m_heatmapImage.width() == m_heatmapGridSize &&
        m_heatmapImage.height() == m_heatmapGridSize) {
        return;
    }

    m_heatmapImage = QImage(m_heatmapGridSize, m_heatmapGridSize, QImage::Format_Grayscale8);
    if (m_heatmapImage.isNull()) {
        return;
    }
    m_heatmapImage.fill(Qt::black);
}

void UnifiedGridRenderer::ensureHeatmapPaletteImage() {
    if (!m_heatmapPaletteImage.isNull()) {
        return;
    }

    const int width = 512;
    const int height = 1;
    // Use ARGB32 format which matches QRgb (0xAARRGGBB) layout
    m_heatmapPaletteImage = QImage(width, height, QImage::Format_ARGB32);
    if (m_heatmapPaletteImage.isNull()) {
        return;
    }

    auto* row = reinterpret_cast<QRgb*>(m_heatmapPaletteImage.scanLine(0));
    for (int i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const bool isAsk = (i >= width / 2);
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float boost = std::pow(std::clamp(localT, 0.0f, 1.0f), 0.8f);
        const int base = static_cast<int>(boost * 255.0f);
        // Bids (first half): GREEN, Asks (second half): RED
        const int r = isAsk ? base : static_cast<int>(base * 0.2f);
        const int g = isAsk ? static_cast<int>(base * 0.2f) : base;
        const int b = static_cast<int>(base * 0.25f);
        row[i] = qRgba(r, g, b, 255);
    }
}

int UnifiedGridRenderer::quantizeFontPx(float fontPx) const {
    const int bucketSize = 2;
    const int minPx = 8;
    const int maxPx = 28;
    const int bucket = static_cast<int>(std::floor(fontPx / bucketSize)) * bucketSize;
    return std::clamp(bucket, minPx, maxPx);
}

QString UnifiedGridRenderer::formatLiquidityLabel(double value, bool dollars) const {
    if (value <= 0.0) {
        return QString();
    }

    const double absValue = value;
    double divisor = 1.0;
    QString suffix;
    if (absValue >= 1.0e9) {
        divisor = 1.0e9;
        suffix = "B";
    } else if (absValue >= 1.0e6) {
        divisor = 1.0e6;
        suffix = "M";
    } else if (absValue >= 1.0e3) {
        divisor = 1.0e3;
        suffix = "k";
    }

    double scaled = absValue / divisor;
    QString number;
    if (divisor > 1.0) {
        if (scaled >= 10.0) {
            scaled = std::floor(scaled);
            number = QString::number(scaled, 'f', 0);
        } else {
            scaled = std::floor(scaled * 10.0) / 10.0;
            if (scaled < 0.1) {
                return QString();
            }
            number = QString::number(scaled, 'f', 1);
            if (number.endsWith(".0")) {
                number.chop(2);
            }
        }
    } else if (absValue < 1.0) {
        scaled = std::floor(absValue * 100.0) / 100.0;
        if (scaled < 0.01) {
            return QString();
        }
        number = QString::number(scaled, 'f', 2);
        while (number.endsWith('0')) {
            number.chop(1);
        }
        if (number.endsWith('.')) {
            number.chop(1);
        }
    } else if (absValue < 10.0) {
        scaled = std::floor(absValue * 10.0) / 10.0;
        number = QString::number(scaled, 'f', 1);
        if (number.endsWith(".0")) {
            number.chop(2);
        }
    } else {
        scaled = std::floor(absValue);
        number = QString::number(scaled, 'f', 0);
    }

    if (dollars) {
        return QString("$%1%2").arg(number, suffix);
    }
    return QString("%1%2").arg(number, suffix);
}

void UnifiedGridRenderer::buildHeatmapLabelImage() {
    HeatmapLabelRequest request;
    {
        std::lock_guard<std::mutex> lock(m_heatmapLabelRequestMutex);
        request = m_heatmapLabelRequest;
    }
    if (!request.valid || request.labelSize.isEmpty()) {
        return;
    }

    if (!m_heatmapLiquidityAvailable || m_heatmapTickSize <= 0.0 || m_heatmapGridSize <= 0) {
        return;
    }

    const int extraWidth = static_cast<int>(std::ceil(request.cellW));
    QImage labelImage(QSize(request.labelSize.width() + extraWidth, request.labelSize.height()),
                      QImage::Format_ARGB32_Premultiplied);
    labelImage.fill(Qt::transparent);

    QPainter painter(&labelImage);
    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);
    font.setPixelSize(request.fontPx);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 230));

    const int cellsX = static_cast<int>(std::ceil(request.srcRect.width())) + 1;
    const int cellsY = static_cast<int>(std::ceil(request.srcRect.height()));
    const bool dollars = (m_liquidityLabelMode != 0);

    {
        std::lock_guard<std::mutex> lock(m_heatmapLiquidityMutex);
        if (m_heatmapLiquidityRing.size() != static_cast<size_t>(m_heatmapGridSize) * m_heatmapGridSize) {
            return;
        }
        for (int j = 0; j < cellsY; ++j) {
            int texY = request.startY + j;
            if (texY < 0) {
                texY = m_heatmapGridSize + (texY % m_heatmapGridSize);
            }
            texY = texY % m_heatmapGridSize;
            if (texY < 0 || texY >= m_heatmapGridSize) {
                continue;
            }
            const double price = m_heatmapMaxPrice - (static_cast<double>(texY) * m_heatmapTickSize);
            for (int i = 0; i < cellsX; ++i) {
                int texX = request.startX + i;
                if (texX < 0) {
                    texX = m_heatmapGridSize + (texX % m_heatmapGridSize);
                }
                texX = texX % m_heatmapGridSize;
                if (texX < 0 || texX >= m_heatmapGridSize) {
                    continue;
                }
                const uint16_t raw = m_heatmapLiquidityRing[static_cast<size_t>(texY) * m_heatmapGridSize + texX];
                if (raw == 0) {
                    continue;
                }
                const double scale = (m_heatmapLiquidityScales.size() == static_cast<size_t>(m_heatmapGridSize))
                    ? std::max(1e-12, m_heatmapLiquidityScales[texX])
                    : 1.0;
                double value = static_cast<double>(raw) * scale;
                if (dollars) {
                    value *= price;
                }
                const QString label = formatLiquidityLabel(value, dollars);
                if (label.isEmpty()) {
                    continue;
                }

                const float px = static_cast<float>(i) * request.cellW;
                const float py = static_cast<float>(j) * request.cellH;
                const QRectF cellRect(px, py, request.cellW, request.cellH);
                painter.drawText(cellRect, Qt::AlignCenter, label);
            }
        }
    }
    painter.end();

    {
        std::lock_guard<std::mutex> lock(m_heatmapLabelMutex);
        m_heatmapLabelImage = labelImage;
        m_heatmapLabelBuiltStartX = request.startX;
        m_heatmapLabelBuiltStartY = request.startY;
    }
    m_heatmapLabelVersion.fetch_add(1);
    update();
}

// ===== QML DATA API =====
// Methods for data input and manipulation from QML
void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
void UnifiedGridRenderer::togglePerformanceOverlay() { /* No-op: SentinelMonitor removed */ }
void UnifiedGridRenderer::fitHeatmapToDataRange() {
    if (!m_viewState || m_heatmapAppendMs <= 0 || m_heatmapGridSize <= 0) {
        return;
    }
    if (m_heatmapLastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return;
    }
    const int64_t bufferSpanMs = std::max<int64_t>(
        1, static_cast<int64_t>(m_heatmapGridSize) * m_heatmapAppendMs);
    const int64_t dataEnd = m_heatmapLastSliceStartMs + m_heatmapAppendMs;
    const int64_t dataStart = dataEnd - bufferSpanMs;
    if (dataEnd <= dataStart) {
        return;
    }
    if (m_viewState->isAutoScrollEnabled()) {
        m_viewState->enableAutoScroll(false);
    }
    m_viewState->setViewport(dataStart, dataEnd, m_heatmapMinPrice, m_heatmapMaxPrice);
    update();
}

// ===== QML PROPERTY GETTERS =====
// Read-only property access for QML bindings
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

// ===== QML DEBUG API =====
// Debug and monitoring methods for QML
QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Size:%1x%2").arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
QString UnifiedGridRenderer::getPerformanceStats() const { return "N/A (SentinelMonitor removed)"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_currentFps.load(); }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

// ===== GPU STATS DEBUG API =====
QString UnifiedGridRenderer::getTextureSize() const {
    if (m_useGpuHeatmap && m_heatmapGridSize > 0) {
        return QString("%1x%2").arg(m_heatmapGridSize).arg(m_heatmapGridSize);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
    if (m_useGpuHeatmap && m_heatmapGridSize > 0) {
        // Grayscale8 = 1 byte per pixel
        qint64 bytes = static_cast<qint64>(m_heatmapGridSize) * m_heatmapGridSize;
        double mb = bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
    if (m_useGpuHeatmap) {
        return "Grayscale8";
    }
    return "N/A";
}

double UnifiedGridRenderer::getUploadBandwidth() const {
    return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
    if (m_useGpuHeatmap && m_heatmapGridSize > 0) {
        return QString("%1/%2").arg(m_heatmapWriteColumn).arg(m_heatmapGridSize);
    }
    return "N/A";
}

int UnifiedGridRenderer::getDirtyRegionCount() const {
    if (m_useGpuHeatmap) {
        std::lock_guard<std::mutex> lock(m_heatmapUploadMutex);
        return static_cast<int>(m_heatmapPendingColumns.size());
    }
    return 0;
}

// ===== QT EVENT HANDLERS =====
// Mouse and wheel event handling for user interaction
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
            m_viewState->isTimeWindowValid()) {
            const int64_t timeRange = m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart();
            const double priceRange = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
            if (timeRange != 0 && priceRange != 0.0) {
                const double timePixelsToMs = static_cast<double>(timeRange) / static_cast<double>(width());
                const double pricePixelsToUnits = priceRange / static_cast<double>(height());
                const double timeDeltaF = (-pan.x() * timePixelsToMs);
                const int64_t timeDelta = static_cast<int64_t>(std::floor(timeDeltaF));
                const double priceDelta = pan.y() * pricePixelsToUnits;
                m_autoScrollLagMs += timeDelta;
                if (timeDelta != 0 || priceDelta != 0.0) {
                    const qint64 newStart = m_viewState->getVisibleTimeStart() + timeDelta;
                    const qint64 newEnd = m_viewState->getVisibleTimeEnd() + timeDelta;
                    const double newMin = m_viewState->getMinPrice() + priceDelta;
                    const double newMax = m_viewState->getMaxPrice() + priceDelta;
                    m_viewState->setViewport(newStart, newEnd, newMin, newMax);
                }
            }
            m_viewState->handlePanEnd(false);
            m_panSyncPending = true;
            update();
            panAppliedToAuto = true;
        } else {
            m_viewState->handlePanEnd(true);
        }
        event->accept();
        if (m_viewState->isAutoScrollEnabled() &&
            m_heatmapAppendMs > 0 &&
            m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min() &&
            !panAppliedToAuto) {
            const int64_t spanMs = std::max<int64_t>(1, m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
            const int64_t padMs = static_cast<int64_t>(spanMs * m_autoScrollPaddingFrac);
            m_autoScrollLagMs = (m_viewState->getVisibleTimeEnd() - (m_heatmapLastSliceStartMs + m_heatmapAppendMs)) - padMs;
        }
        // Keep visual pan until resync arrives to avoid snap-back
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
