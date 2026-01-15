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
#include <QDateTime>
#include <QTimer>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <cmath>
// #include <algorithm>
#include <algorithm>
// #include <cmath>

// New modular architecture includes
#include "render/GridTypes.hpp"
#include "render/GridViewState.hpp"
#include "render/GridSceneNode.hpp" 
#include "render/DataProcessor.hpp"
#include "render/IRenderStrategy.hpp"
#include "render/IDataAccessor.hpp"
#include "render/HeatmapIntensityNode.hpp"
#include "render/strategies/HeatmapStrategy.hpp"
#include "render/strategies/TradeFlowStrategy.hpp"
#include "render/strategies/TradeBubbleStrategy.hpp"
#include "render/strategies/CandleStrategy.hpp"

namespace {
uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float rand01(uint32_t seed) {
    return static_cast<float>(hash32(seed) & 0xFFFFu) / 65535.0f;
}

float bandShape(float value, float spacing) {
    const float phase = std::fmod(value, spacing) / spacing;
    const float tri = 1.0f - std::abs(phase - 0.5f) * 2.0f;
    return std::max(0.0f, tri);
}

float computeDummyIntensity(int x, int y, int grid, int mid) {
    const int segment = 128;
    const int slowSegment = 512;
    const float majorSpacing = 64.0f;
    const float minorSpacing = 12.0f;

    const bool isAsk = y < mid;
    const float dist = std::abs(static_cast<float>(y) - static_cast<float>(mid)) / static_cast<float>(mid);
    const float falloff = std::exp(-dist * 1.6f);
    const float majorBand = bandShape(static_cast<float>(y), majorSpacing);
    const float minorBand = bandShape(static_cast<float>(y), minorSpacing);
    const int bandIndex = static_cast<int>(static_cast<float>(y) / majorSpacing);
    const float bandSeed = rand01(static_cast<uint32_t>(bandIndex) * 2654435761u);
    const float bandBias = 0.15f + 0.85f * (0.65f * majorBand + 0.35f * minorBand);
    const float density = (isAsk ? 0.70f : 0.78f) * (0.6f + 0.4f * (1.0f - dist));

    const int seg = x / segment;
    const int segSlow = x / slowSegment;
    const uint32_t seed = static_cast<uint32_t>(bandIndex) * 73856093u
        ^ static_cast<uint32_t>(seg) * 19349663u
        ^ static_cast<uint32_t>(segSlow) * 83492791u
        ^ static_cast<uint32_t>(isAsk ? 0xA5A5u : 0x5A5Au);

    const float gate = rand01(seed);
    const float strength = rand01(seed ^ 0x9E3779B9u);
    const float active = (gate < density) ? 1.0f : 0.0f;

    const float timeWave = 0.85f + 0.15f * std::sin((x * 0.0025f) + (y * 0.006f));
    const float bandLine = (0.10f + 0.20f * bandSeed) * bandBias * falloff;
    float intensity = bandLine + active * (0.35f + 0.65f * strength) * falloff * timeWave;

    const float burstGate = rand01(seed ^ 0xC2B2AE35u);
    if (burstGate > 0.992f) {
        intensity = std::max(intensity, 0.75f * falloff);
    }

    const float hot = std::pow(std::min(1.0f, intensity), 0.85f);
    return std::clamp(hot, 0.0f, 1.0f);
}
} // namespace
UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
    , m_rootTransformNode(nullptr)
    , m_needsDataRefresh(false)
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
    // REMOVED: Trade buffering in UGR. Strategies now pull from DataCache.
    /*
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_recentTrades.push_back(trade);
        if (m_recentTrades.size() > 1000) {
            m_recentTrades.erase(m_recentTrades.begin(), m_recentTrades.begin() + 100); // Remove oldest 100
        }
    }
    */
    
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), "onTradeReceived", 
                                 Qt::QueuedConnection, Q_ARG(Trade, trade));
    }
}

// onLiveOrderBookUpdated(QString) removed: legacy pass-through; MainWindow connects Core→DataProcessor directly

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    if (m_viewState) {
        m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
    }
    
    m_transformDirty.store(true);
    update();
    
    sLog_Debug("UNIFIED RENDERER VIEWPORT Time:[" << startTimeMs << "-" << endTimeMs << "]"
               << "Price:[$" << minPrice << "-$" << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
    if (!m_viewState || !m_dataProcessor) return;
    // Trigger data processor to recalculate visible cells for new viewport
    QMetaObject::invokeMethod(m_dataProcessor.get(), "updateVisibleCells", Qt::QueuedConnection);
    // Mark transform dirty for rendering update
    m_transformDirty.store(true);
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
        m_transformDirty.store(true);
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

void UnifiedGridRenderer::updateVisibleCells() {
    // TODO: Optimize this function
    
    // Non-blocking: consume latest snapshot, request async recompute if needed
    if (m_dataProcessor) {
        // Try to grab the latest published cells without blocking the worker
        auto snapshot = m_dataProcessor->getPublishedCellsSnapshot();
        if (snapshot) {
            m_visibleCells = snapshot; // Zero-copy share
        }
    } else {
        m_visibleCells.reset();
    }
    // Avoid writing viewport state from the render thread; size is handled in geometryChanged
}

void UnifiedGridRenderer::updateVolumeProfile() {
    // TODO: Implement volume profile from liquidity time series
    m_volumeProfile.clear();
}

// Property setters
void UnifiedGridRenderer::setRenderMode(RenderMode mode) {
    if (m_renderMode != mode) {
        m_renderMode = mode;
        m_geometryDirty.store(true);
        update();
        emit renderModeChanged();
    }
}

void UnifiedGridRenderer::setShowVolumeProfile(bool show) {
    if (m_showVolumeProfile != show) {
        m_showVolumeProfile = show;
        m_materialDirty.store(true);
        update();
        emit showVolumeProfileChanged();
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
        m_materialDirty.store(true);
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
        m_materialDirty.store(true);
        update();
        emit minVolumeFilterChanged();
    }
}

void UnifiedGridRenderer::setMinBubbleRadius(double radius) {
    if (m_minBubbleRadius != radius && radius > 0) {
        m_minBubbleRadius = radius;
        if (m_tradeBubbleStrategy) {
            auto* bubbleStrategy = static_cast<TradeBubbleStrategy*>(m_tradeBubbleStrategy.get());
            bubbleStrategy->setMinBubbleRadius(static_cast<float>(radius));
        }
        m_materialDirty.store(true);
        update();
        emit minBubbleRadiusChanged();
    }
}

void UnifiedGridRenderer::setMaxBubbleRadius(double radius) {
    if (m_maxBubbleRadius != radius && radius > 0) {
        m_maxBubbleRadius = radius;
        if (m_tradeBubbleStrategy) {
            auto* bubbleStrategy = static_cast<TradeBubbleStrategy*>(m_tradeBubbleStrategy.get());
            bubbleStrategy->setMaxBubbleRadius(static_cast<float>(radius));
        }
        m_materialDirty.store(true);
        update();
        emit maxBubbleRadiusChanged();
    }
}

void UnifiedGridRenderer::setBubbleOpacity(double opacity) {
    if (m_bubbleOpacity != opacity && opacity >= 0.0 && opacity <= 1.0) {
        m_bubbleOpacity = opacity;
        if (m_tradeBubbleStrategy) {
            auto* bubbleStrategy = static_cast<TradeBubbleStrategy*>(m_tradeBubbleStrategy.get());
            bubbleStrategy->setBubbleOpacity(static_cast<float>(opacity));
        }
        m_materialDirty.store(true);
        update();
        emit bubbleOpacityChanged();
    }
}

void UnifiedGridRenderer::setShowHeatmapLayer(bool show) {
    if (m_showHeatmapLayer != show) {
        m_showHeatmapLayer = show;
        m_geometryDirty.store(true);
        update();
        emit showHeatmapLayerChanged();
    }
}

void UnifiedGridRenderer::setShowTradeBubbleLayer(bool show) {
    if (m_showTradeBubbleLayer != show) {
        m_showTradeBubbleLayer = show;
        m_geometryDirty.store(true);
        update();
        emit showTradeBubbleLayerChanged();
    }
}

void UnifiedGridRenderer::setShowTradeFlowLayer(bool show) {
    if (m_showTradeFlowLayer != show) {
        m_showTradeFlowLayer = show;
        m_geometryDirty.store(true);
        update();
        emit showTradeFlowLayerChanged();
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
    
    // Clear rendering data
    m_visibleCells.reset();
    m_volumeProfile.clear();
    
    m_geometryDirty.store(true);
    update();
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_dataProcessor && resolution > 0) {
        m_dataProcessor->setPriceResolution(resolution);
        m_geometryDirty.store(true);
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
        m_geometryDirty.store(true);
        update();
        emit timeframeChanged();
    }
}

void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); m_transformDirty.store(true); m_appendPending.store(true); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); m_transformDirty.store(true); m_appendPending.store(true); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); m_transformDirty.store(true); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); m_transformDirty.store(true); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); m_transformDirty.store(true); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); m_transformDirty.store(true); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); m_transformDirty.store(true); update(); } }

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        m_transformDirty.store(true);
        update();
        emit autoScrollEnabledChanged();
        sLog_Render("Auto-scroll: "<< (enabled ? "ENABLED" : "DISABLED"));
        if (enabled && m_viewState->isTimeWindowValid() && m_heatmapAppendMs > 0 &&
            m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min()) {
            const int64_t spanMs = std::max<int64_t>(1, m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
            const int64_t padMs = static_cast<int64_t>(spanMs * m_autoScrollPaddingFrac);
            m_autoScrollLagMs = (m_viewState->getVisibleTimeEnd() - (m_heatmapLastSliceStartMs + m_heatmapAppendMs)) - padMs;
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
    m_useDummyHeatmap = qEnvironmentVariableIsSet("SENTINEL_DUMMY_HEATMAP");
    sLog_App("Dummy heatmap: " << (m_useDummyHeatmap ? "ENABLED" : "disabled"));
    if (m_useDummyHeatmap) {
        initDummyHeatmap();
    }

    m_useGpuHeatmap = !m_useDummyHeatmap && qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP");
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
    connect(m_dataProcessor.get(), &DataProcessor::dataUpdated, 
            this, [this]() { 
                // If a pan sync is pending, clear the visual offset now (GUI thread)
                if (m_panSyncPending && m_viewState) {
                    m_viewState->clearPanVisualOffset();
                    m_panSyncPending = false;
                    m_transformDirty.store(true);
                }
                // Non-blocking refresh: new data arrived, append cells
                m_appendPending.store(true);
                update();
            }, Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::viewportInitialized,
            this, &UnifiedGridRenderer::viewportChanged, Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::heatmapColumnReady,
            this,
            [this](int64_t sliceStartMs,
                   int64_t sliceEndMs,
                   int64_t timeframeMs,
                   double minPrice,
                   double maxPrice,
                   double tickSize,
                   const QByteArray& column) {
                Q_UNUSED(sliceEndMs);
                const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
                if (!m_useGpuHeatmap) {
                    if (m_useDummyHeatmap) {
                        if (debug) {
                            sLog_Render("GPU HEATMAP DROP: enabled=" << m_useGpuHeatmap
                                        << " grid=" << m_heatmapGridSize
                                        << " bytes=" << column.size());
                        }
                        return;
                    }
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

                int step = 1;
                if (m_heatmapLastSliceStartMs != std::numeric_limits<int64_t>::min() &&
                    m_heatmapAppendMs > 0) {
                    const int64_t dt = sliceStartMs - m_heatmapLastSliceStartMs;
                    if (dt > 0) {
                        const int64_t rawStep = dt / m_heatmapAppendMs;
                        step = static_cast<int>(std::max<int64_t>(1, rawStep));
                    }
                }

                QByteArray fillColumn;
                if (!m_heatmapHaveLastColumn) {
                    fillColumn = QByteArray(column.size(), 0);
                } else {
                    fillColumn = m_heatmapLastColumnData;
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
                m_heatmapLastSliceStartMs = sliceStartMs;
                m_heatmapLastColumnData = column;
                m_heatmapHaveLastColumn = true;

                m_heatmapLastAppendMs = m_heatmapClock.elapsed();
                if (debug) {
                    sLog_Render("GPU HEATMAP ENQUEUE: col=" << m_heatmapWriteColumn
                                << " step=" << step
                                << " tf=" << timeframeMs
                                << " range=$" << m_heatmapMinPrice << "-$" << m_heatmapMaxPrice);
                }
                if (!m_heatmapViewportInitialized && m_viewState && timeframeMs > 0 && m_heatmapGridSize > 0) {
                    const int64_t spanMs = static_cast<int64_t>(m_heatmapGridSize - 1) * timeframeMs;
                    const int64_t viewStart = sliceStartMs - spanMs;
                    const int64_t viewEnd = sliceStartMs + timeframeMs;
                    if (viewEnd > viewStart) {
                        m_viewState->setViewport(viewStart, viewEnd, m_heatmapMinPrice, m_heatmapMaxPrice);
                        m_heatmapViewportInitialized = true;
                        m_transformDirty.store(true);
                        if (debug) {
                            sLog_Render("GPU HEATMAP VIEWPORT INIT: [" << viewStart << "-" << viewEnd
                                        << "] $" << m_heatmapMinPrice << "-$" << m_heatmapMaxPrice);
                        }
                    }
                }

                if (m_viewState && m_viewState->isAutoScrollEnabled() && timeframeMs > 0 && !m_viewState->isDragging()) {
                    const int64_t spanMs = std::max<int64_t>(1, m_viewState->getVisibleTimeEnd() - m_viewState->getVisibleTimeStart());
                    const int64_t maxSpanMs = static_cast<int64_t>(m_heatmapGridSize) * timeframeMs;
                    const int64_t clampedSpanMs = std::min(spanMs, maxSpanMs);
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
                }
                update();
            },
            Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::heatmapRangeReset,
            this,
            [this](double minPrice, double maxPrice, double tickSize, int gridHeight) {
                if (!m_useGpuHeatmap) {
                    if (m_useDummyHeatmap) {
                        return;
                    }
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

    if (m_useDummyHeatmap) {
        const qint64 startTime = 0;
        const qint64 endTime = static_cast<qint64>(m_dummyGridSize);
        const double minPrice = 0.0;
        const double maxPrice = static_cast<double>(m_dummyGridSize);
        m_viewState->setViewport(startTime, endTime, minPrice, maxPrice);
    }

    // Dependencies will be set later when setDataCache() is called
    // Set ViewState immediately as it's available
    QMetaObject::invokeMethod(m_dataProcessor.get(), [this]() {
        m_dataProcessor->setGridViewState(m_viewState.get());
    }, Qt::QueuedConnection);
    
    m_heatmapStrategy = std::make_unique<HeatmapStrategy>();
    m_tradeFlowStrategy = std::make_unique<TradeFlowStrategy>();
    m_tradeBubbleStrategy = std::make_unique<TradeBubbleStrategy>();
    m_candleStrategy = std::make_unique<CandleStrategy>();
    
    // Initialize bubble strategy with default configuration
    auto* bubbleStrategy = static_cast<TradeBubbleStrategy*>(m_tradeBubbleStrategy.get());
    bubbleStrategy->setMinBubbleRadius(static_cast<float>(m_minBubbleRadius));
    bubbleStrategy->setMaxBubbleRadius(static_cast<float>(m_maxBubbleRadius));
    bubbleStrategy->setBubbleOpacity(static_cast<float>(m_bubbleOpacity));
    
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::viewportChanged);
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::onViewportChanged);
    connect(m_viewState.get(), &GridViewState::panVisualOffsetChanged, this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(m_viewState.get(), &GridViewState::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    
    if (!m_useDummyHeatmap) {
        QMetaObject::invokeMethod(
            m_dataProcessor.get(),
            &DataProcessor::startProcessing,
            Qt::QueuedConnection);
    }

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
            const qint64 nowMs = m_heatmapClock.elapsed();
            const qint64 delta = nowMs - m_heatmapLastAppendMs;
            const float frac = std::clamp(static_cast<float>(delta) / static_cast<float>(m_heatmapAppendMs),
                                          0.0f, 1.0f);
            const float offset = (static_cast<float>(m_heatmapWriteColumn) + frac) /
                                 static_cast<float>(m_heatmapGridSize);
            m_heatmapTimeOffset.store(offset);
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

IRenderStrategy* UnifiedGridRenderer::getCurrentStrategy() const {
    switch (m_renderMode) {
        case RenderMode::LiquidityHeatmap:
        case RenderMode::OrderBookDepth:  // Similar to heatmap
            return m_heatmapStrategy.get();
            
        case RenderMode::TradeFlow:
            return m_tradeFlowStrategy.get();
            
        case RenderMode::TradeBubbles:
            return m_tradeBubbleStrategy.get();
            
        case RenderMode::VolumeCandles:
            return m_candleStrategy.get();
    }
    
    return m_heatmapStrategy.get(); // Default fallback
}

namespace {
    inline Viewport buildViewport(const GridViewState* view, double w, double h) {
        if (view) {
            return Viewport{view->getVisibleTimeStart(), view->getVisibleTimeEnd(), view->getMinPrice(), view->getMaxPrice(), w, h};
        }
        return Viewport{0, 0, 0.0, 0.0, w, h};
    }

    // UGR implementation of data accessor (Phase 3/4 - owns frame config)
    class UGRDataAccessor : public IDataAccessor {
    private:
        UnifiedGridRenderer* m_ugr;
        IGridDataSource* m_dataSource;          // Access to live data
        DataProcessor* m_dataProcessor;  // Access to processed cell data
        double m_intensityScale;
        double m_minVolumeFilter;
        int m_maxCells;
        Viewport m_viewport;
        std::string m_symbol;            // Current symbol context
        mutable std::vector<Trade> m_tradeCache;  // Cache trades for accessor lifetime
    
    public:
        explicit UGRDataAccessor(UnifiedGridRenderer* ugr,
                                 double intensityScale_,
                                 double minVolumeFilter_,
                                 int maxCells_,
                                 const Viewport& viewport_,
                                 IGridDataSource* dataSource,
                                 DataProcessor* processor,
                                 const std::string& symbol = "BTC-USD") 
            : m_ugr(ugr)
            , m_dataSource(dataSource)
            , m_dataProcessor(processor)
            , m_intensityScale(intensityScale_)
            , m_minVolumeFilter(minVolumeFilter_)
            , m_maxCells(maxCells_)
            , m_viewport(viewport_)
            , m_symbol(symbol) {}
        
        std::shared_ptr<const std::vector<CellInstance>> getVisibleCells() const override {
            if (m_dataProcessor) {
                return m_dataProcessor->getPublishedCellsSnapshot();
            }
            return nullptr;
        }
        
        const std::vector<Trade>& getRecentTrades() const override {
            if (m_dataSource) {
                // Cache the trades for the lifetime of this accessor instance.
                // IGridDataSource::getRecentTrades returns by value, so we store it in a member
                // to safely return a reference as required by the IDataAccessor interface.
                m_tradeCache = m_dataSource->getRecentTrades(m_symbol);
                return m_tradeCache;
            }
            // Fallback to empty if no cache (shouldn't happen in prod)
            static const std::vector<Trade> empty;
            return empty;
        }
        
        Viewport getViewport() const override {
            return m_viewport;
        }
        
        double getIntensityScale() const override {
            return m_intensityScale;
        }
        
        double getMinVolumeFilter() const override {
            return m_minVolumeFilter;
        }
        
        int getMaxCells() const override {
            return m_maxCells;
        }
    };
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)
    if (width() <= 0 || height() <= 0) { 
        return oldNode;
    }

    if (m_useDummyHeatmap) {
        auto* texNode = static_cast<HeatmapIntensityNode*>(oldNode);
        if (!texNode) {
            texNode = new HeatmapIntensityNode();
            m_dummyTextureDirty = true;
        }

        if (m_dummyTextureDirty) {
            ensureDummyImage();
            ensureDummyPaletteImage();
            auto* intensityTexture = window()->createTextureFromImage(m_dummyImage);
            if (!intensityTexture) {
                QImage fallback = m_dummyImage.convertToFormat(QImage::Format_RGBA8888);
                intensityTexture = window()->createTextureFromImage(fallback);
            }
            auto* paletteTexture = window()->createTextureFromImage(m_dummyPaletteImage);
            if (intensityTexture && paletteTexture) {
                intensityTexture->setFiltering(QSGTexture::Nearest);
                paletteTexture->setFiltering(QSGTexture::Linear);
                texNode->setTextures(intensityTexture, paletteTexture);
                texNode->setGamma(1.05f);
                texNode->setContrast(1.15f);
                m_dummyTextureDirty = false;
            }
        }

        const QRectF bounds = boundingRect();
        texNode->setRect(bounds);

        if (m_viewState && m_viewState->isTimeWindowValid()) {
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

            if (timeEndF > timeStartF && maxPriceF > minPriceF) {
                const double maxCoord = static_cast<double>(m_dummyGridSize);
                const double windowW = timeEndF - timeStartF;
                const double windowH = maxPriceF - minPriceF;

                const double srcW = std::clamp(windowW, 1.0, maxCoord);
                const double srcH = std::clamp(windowH, 1.0, maxCoord);

                const double clampedStart = (windowW >= maxCoord)
                    ? 0.0
                    : std::clamp(timeStartF, 0.0, maxCoord - srcW);
                const double clampedMinPrice = (windowH >= maxCoord)
                    ? 0.0
                    : std::clamp(minPriceF, 0.0, maxCoord - srcH);

                const double srcX = clampedStart;
                const double srcY = std::clamp(maxCoord - (clampedMinPrice + srcH), 0.0, maxCoord - srcH);
                texNode->setSourceRect(QRectF(srcX, srcY, srcW, srcH));
            } else {
                texNode->setSourceRect(QRectF(0, 0, m_dummyGridSize, m_dummyGridSize));
            }
        } else {
            texNode->setSourceRect(QRectF(0, 0, m_dummyGridSize, m_dummyGridSize));
        }

        texNode->setTimeOffset(m_dummyTimeOffset.load());

        const bool labelsEnabled = !qEnvironmentVariableIsSet("SENTINEL_DUMMY_LABELS_OFF");
        if (labelsEnabled && !bounds.isEmpty()) {
            const QRectF srcRect = texNode->getSourceRect();
            const QSize labelSize = bounds.size().toSize();
            if (!labelSize.isEmpty()) {
                const int gridSize = m_dummyGridSize;
                const float baseX = static_cast<float>(srcRect.x()) + (m_dummyTimeOffset.load() * gridSize);
                const int startX = static_cast<int>(std::floor(baseX));
                const float fracX = baseX - static_cast<float>(startX);

                const float baseY = static_cast<float>(srcRect.y());
                const int startY = static_cast<int>(std::floor(baseY));
                const float fracY = baseY - static_cast<float>(startY);

                if (srcRect != m_dummyLabelSourceRect || labelSize != m_dummyLabelPixelSize || startX != m_dummyLabelStartX) {
                    m_dummyLabelSourceRect = srcRect;
                    m_dummyLabelPixelSize = labelSize;
                    m_dummyLabelStartX = startX;
                    m_dummyLabelDirty = true;
                }

                const float cellW = static_cast<float>(bounds.width()) / std::max(1.0, srcRect.width());
                const float cellH = static_cast<float>(bounds.height()) / std::max(1.0, srcRect.height());
                const float minCell = std::min(cellW, cellH);
                const float labelThreshold = 18.0f;
                const float shiftPx = fracX * cellW;
                const float shiftPy = fracY * cellH;

                auto* labelNode = dynamic_cast<QSGSimpleTextureNode*>(texNode->firstChild());
                if (minCell >= labelThreshold && !m_dummyImage.isNull() && window()) {
                    if (!labelNode) {
                        labelNode = new QSGSimpleTextureNode();
                        texNode->appendChildNode(labelNode);
                    }

                    if (m_dummyLabelDirty) {
                        DummyLabelRequest request;
                        request.srcRect = srcRect;
                        request.labelSize = labelSize;
                        request.startX = startX;
                        request.startY = startY;
                        request.cellW = cellW;
                        request.cellH = cellH;
                        request.valid = true;
                        {
                            std::lock_guard<std::mutex> lock(m_dummyLabelRequestMutex);
                            m_dummyLabelRequest = request;
                        }
                        QMetaObject::invokeMethod(this, [this]() { buildDummyLabelImage(); }, Qt::QueuedConnection);
                        m_dummyLabelDirty = false;
                    }

                    int labelVersion = m_dummyLabelVersion.load();
                    if (labelVersion != m_dummyLabelTextureVersion) {
                        QImage labelImage;
                        {
                            std::lock_guard<std::mutex> lock(m_dummyLabelMutex);
                            labelImage = m_dummyLabelImage;
                        }
                        if (!labelImage.isNull()) {
                            auto* labelTex = window()->createTextureFromImage(labelImage);
                            labelTex->setFiltering(QSGTexture::Nearest);
                            labelNode->setTexture(labelTex);
                            labelNode->setOwnsTexture(true);
                            m_dummyLabelTextureVersion = labelVersion;
                        }
                    }

                    if (labelNode && labelNode->texture()) {
                        const QSize texSize = labelNode->texture()->textureSize();
                        labelNode->setRect(QRectF(bounds.x() - shiftPx, bounds.y() - shiftPy, texSize.width(), texSize.height()));
                    }
                } else if (labelNode) {
                    labelNode->setRect(QRectF());
                    m_dummyLabelDirty = true;
                }
            }
        }

        std::vector<DummyPendingColumn> pendingUploads;
        {
            std::lock_guard<std::mutex> lock(m_dummyUploadMutex);
            if (!m_dummyPendingColumns.empty()) {
                pendingUploads.swap(m_dummyPendingColumns);
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
        texNode->setRect(bounds);

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
                texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
                texNode->setTimeOffset(0.0f);
            } else if (timeEndF > timeStartF && maxPriceF > minPriceF) {
                const double maxCoord = static_cast<double>(m_heatmapGridSize);
                const double windowW = timeEndF - timeStartF;
                const double windowH = maxPriceF - minPriceF;

                const double srcW = std::clamp(windowW / m_heatmapAppendMs, 1.0, maxCoord);
                const double srcH = std::clamp(windowH / m_heatmapTickSize, 1.0, maxCoord);

                const double timeMax = m_heatmapTimeOriginMs + (maxCoord - srcW) * m_heatmapAppendMs;
                const double priceMax = m_heatmapMaxPrice - (srcH * m_heatmapTickSize);
                const double clampedStart = std::clamp(timeStartF, static_cast<double>(m_heatmapTimeOriginMs), timeMax);
                const double clampedMinPrice = std::clamp(minPriceF, m_heatmapMinPrice, priceMax);

                const double srcX = (clampedStart - m_heatmapTimeOriginMs) / m_heatmapAppendMs;
                const double srcY = (m_heatmapMaxPrice - (clampedMinPrice + srcH * m_heatmapTickSize)) /
                                    m_heatmapTickSize;
                texNode->setSourceRect(QRectF(srcX, srcY, srcW, srcH));
            } else {
                texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
            }
        } else {
            texNode->setSourceRect(QRectF(0, 0, m_heatmapGridSize, m_heatmapGridSize));
        }

        if (!qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_FORCE_FULL")) {
            texNode->setTimeOffset(m_heatmapTimeOffset.load());
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

    QElapsedTimer timer;
    timer.start();

    auto* sceneNode = static_cast<GridSceneNode*>(oldNode); // cast the old node to a GridSceneNode
    bool isNewNode = !sceneNode; // check if the node is new
    if (isNewNode) { // if the node is new, create a new GridSceneNode
        sceneNode = new GridSceneNode();
    }
    
    qint64 cacheUs = 0; // cache time in microseconds
    qint64 contentUs = 0; // content time in microseconds
    size_t cellsCount = 0; // number of cells
    
    // TODO: REMOVE COMMENTS AFTER IMPLEMENTING THE 4 DIRTY FLAGS SYSTEM
    //  FOUR DIRTY FLAGS SYSTEM - No mutex needed, atomic exchange
    
    // Priority: geometry → append → material → transform
    if (m_geometryDirty.exchange(false) || isNewNode) {
        sLog_Render("FULL GEOMETRY REBUILD (mode/LOD/timeframe changed)");
        QElapsedTimer cacheTimer; cacheTimer.start();
        updateVisibleCells();
        cacheUs = cacheTimer.nsecsElapsed() / 1000;

        Viewport vp = buildViewport(m_viewState.get(), static_cast<double>(width()), static_cast<double>(height()));
        // TODO: Get actual symbol from somewhere (View State or Prop). For now defaulting to BTC-USD as in scaffolding.
        UGRDataAccessor accessor(
            this,
            m_intensityScale,
            m_minVolumeFilter,
            m_maxCells,
            vp,
            m_dataSource,
            m_dataProcessor.get(),
            "BTC-USD"
        );

        QElapsedTimer contentTimer; contentTimer.start();
        sceneNode->updateLayeredContent(&accessor,
                                       m_heatmapStrategy.get(), m_showHeatmapLayer,
                                       m_tradeBubbleStrategy.get(), m_showTradeBubbleLayer,
                                       m_tradeFlowStrategy.get(), m_showTradeFlowLayer);
        contentUs = contentTimer.nsecsElapsed() / 1000;

        if (m_showVolumeProfile) {
            updateVolumeProfile();
            sceneNode->updateVolumeProfile(m_volumeProfile);
        }
        sceneNode->setShowVolumeProfile(m_showVolumeProfile);

        cellsCount = m_visibleCells ? m_visibleCells->size() : 0;
    } else if (m_appendPending.exchange(false)) {
        // sLog_RenderN(5, "APPEND PENDING (rebuild from snapshot)");   
        QElapsedTimer cacheTimer; cacheTimer.start();
        updateVisibleCells();
        cacheUs = cacheTimer.nsecsElapsed() / 1000;

        Viewport vp2 = buildViewport(m_viewState.get(), static_cast<double>(width()), static_cast<double>(height()));
        UGRDataAccessor accessor2(
            this,
            m_intensityScale,
            m_minVolumeFilter,
            m_maxCells,
            vp2,
            m_dataSource,
            m_dataProcessor.get(),
            "BTC-USD"
        );

        QElapsedTimer contentTimer2; contentTimer2.start();
        sceneNode->updateLayeredContent(&accessor2,
                                       m_heatmapStrategy.get(), m_showHeatmapLayer,
                                       m_tradeBubbleStrategy.get(), m_showTradeBubbleLayer,
                                       m_tradeFlowStrategy.get(), m_showTradeFlowLayer);
        contentUs = contentTimer2.nsecsElapsed() / 1000;
        cellsCount = m_visibleCells ? m_visibleCells->size() : 0;
    }

    if (m_materialDirty.exchange(false)) {
        sLog_RenderN(10, "MATERIAL UPDATE (intensity/palette)");
        updateVisibleCells();
        Viewport vp3 = buildViewport(m_viewState.get(), static_cast<double>(width()), static_cast<double>(height()));
        UGRDataAccessor accessor3(
            this,
            m_intensityScale,
            m_minVolumeFilter,
            m_maxCells,
            vp3,
            m_dataSource,
            m_dataProcessor.get(),
            "BTC-USD"
        );

        sceneNode->updateLayeredContent(&accessor3,
                                       m_heatmapStrategy.get(), m_showHeatmapLayer,
                                       m_tradeBubbleStrategy.get(), m_showTradeBubbleLayer,
                                       m_tradeFlowStrategy.get(), m_showTradeFlowLayer);
    }

    if (m_transformDirty.exchange(false) || isNewNode) {
        QMatrix4x4 transform;
        if (m_viewState) {
            QPointF pan = m_viewState->getPanVisualOffset();
            transform.translate(pan.x(), pan.y());
        }
        sceneNode->updateTransform(transform);
        sLog_RenderN(20, "TRANSFORM UPDATE (pan/zoom)");
    }

    const qint64 totalUs = timer.nsecsElapsed() / 1000;
    sLog_RenderN(10, "UGR paint: total=" << totalUs << "microseconds"
                       << "cache=" << cacheUs << "microseconds"
                       << "content=" << contentUs << "microseconds"
                       << "cells=" << cellsCount);

    // DIAGNOSTIC: Check if we have cells but they're not distributed properly
    if (cellsCount > 0 && cellsCount % 100 == 0 && m_visibleCells) {
        std::map<int64_t, size_t> cellsPerTimeSlice;
        for (const auto& cell : *m_visibleCells) {
            cellsPerTimeSlice[cell.timeStart_ms]++;
        }
        sLog_Debug("CELL DISTRIBUTION: " << cellsPerTimeSlice.size() << " time slices, "
                   << "first=" << (cellsPerTimeSlice.empty() ? 0 : cellsPerTimeSlice.begin()->first)
                   << " count=" << (cellsPerTimeSlice.empty() ? 0 : cellsPerTimeSlice.begin()->second));
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

    return sceneNode;
}

void UnifiedGridRenderer::initDummyHeatmap() {
    bool ok = false;
    const int envSize = qgetenv("SENTINEL_DUMMY_GRID").toInt(&ok);
    if (ok && envSize > 0) {
        m_dummyGridSize = envSize;
    }

    if (m_viewState) {
        const qint64 startTime = 0;
        const qint64 endTime = static_cast<qint64>(m_dummyGridSize);
        const double minPrice = 0.0;
        const double maxPrice = static_cast<double>(m_dummyGridSize);
        m_viewState->setViewport(startTime, endTime, minPrice, maxPrice);
    }

    if (!m_dummyAppendTimer) {
        m_dummyAppendTimer = new QTimer(this);
        connect(m_dummyAppendTimer, &QTimer::timeout, this, &UnifiedGridRenderer::appendDummyColumn);
    }

    int appendMs = 100;
    const QByteArray appendEnv = qgetenv("SENTINEL_DUMMY_APPEND_MS");
    if (!appendEnv.isEmpty()) {
        bool appendOk = false;
        const int parsed = appendEnv.toInt(&appendOk);
        if (appendOk) {
            appendMs = parsed;
        }
    }

    m_dummyWriteColumn = 0;
    m_dummyTimeIndex = 0;
    m_dummyAppendMs = appendMs;
    m_dummyClock.start();
    m_dummyLastAppendMs = m_dummyClock.elapsed();
    m_dummyTimeOffset.store(0.0f);

    // Initialize upload bandwidth tracking
    m_uploadTimer.start();
    m_totalBytesUploaded.store(0);
    m_uploadBandwidthMBps.store(0.0);
    m_lastBandwidthUpdate = 0;

    if (appendMs > 0) {
        m_dummyAppendTimer->start(appendMs);
    } else {
        m_dummyAppendTimer->stop();
    }

    if (!m_dummyRenderTimer) {
        m_dummyRenderTimer = new QTimer(this);
        connect(m_dummyRenderTimer, &QTimer::timeout, this, &UnifiedGridRenderer::onDummyRenderTick);
    }
    int renderMs = 16;
    const QByteArray renderEnv = qgetenv("SENTINEL_DUMMY_RENDER_MS");
    if (!renderEnv.isEmpty()) {
        bool renderOk = false;
        const int parsed = renderEnv.toInt(&renderOk);
        if (renderOk) {
            renderMs = parsed;
        }
    }
    if (renderMs > 0) {
        m_dummyRenderTimer->start(renderMs);
    } else {
        m_dummyRenderTimer->stop();
    }
}

void UnifiedGridRenderer::ensureDummyImage() {
    if (!m_dummyImage.isNull() && m_dummyImage.width() == m_dummyGridSize &&
        m_dummyImage.height() == m_dummyGridSize) {
        return;
    }

    m_dummyImage = QImage(m_dummyGridSize, m_dummyGridSize, QImage::Format_ARGB32);
    if (m_dummyImage.isNull()) {
        return;
    }

    const int grid = m_dummyGridSize;
    const int mid = grid / 2;

    for (int y = 0; y < grid; ++y) {
        auto* row = reinterpret_cast<QRgb*>(m_dummyImage.scanLine(y));
        for (int x = 0; x < grid; ++x) {
            const float intensityOut = computeDummyIntensity(x, y, grid, mid);
            const int v = static_cast<int>(intensityOut * 255.0f);
            row[x] = qRgba(v, v, v, 255);
        }
    }
}

void UnifiedGridRenderer::ensureDummyPaletteImage() {
    if (!m_dummyPaletteImage.isNull()) {
        return;
    }

    const int width = 512;
    const int height = 1;
    m_dummyPaletteImage = QImage(width, height, QImage::Format_RGBA8888);
    if (m_dummyPaletteImage.isNull()) {
        return;
    }

    auto* row = reinterpret_cast<QRgb*>(m_dummyPaletteImage.scanLine(0));
    for (int i = 0; i < width; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(width - 1);
        const bool isAsk = (i >= width / 2);
        const float localT = isAsk ? (t - 0.5f) * 2.0f : t * 2.0f;
        const float boost = std::pow(std::clamp(localT, 0.0f, 1.0f), 0.8f);
        const int base = static_cast<int>(boost * 255.0f);
        const int r = isAsk ? base : static_cast<int>(base * 0.25f);
        const int g = isAsk ? static_cast<int>(base * 0.25f) : base;
        const int b = static_cast<int>(base * 0.35f);
        row[i] = qRgba(r, g, b, 255);
    }
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

void UnifiedGridRenderer::appendDummyColumn() {
    if (!m_useDummyHeatmap || m_dummyGridSize <= 0) {
        return;
    }

    if (m_dummyImage.isNull()) {
        ensureDummyImage();
    }

    if (m_dummyImage.isNull()) {
        return;
    }

    const int grid = m_dummyGridSize;
    const int mid = grid / 2;
    const int column = m_dummyWriteColumn;
    const int xSample = static_cast<int>(m_dummyTimeIndex % (static_cast<int64_t>(grid) * 4));

    QByteArray columnData;
    columnData.resize(grid * 4);
    auto* dst = reinterpret_cast<uchar*>(columnData.data());

    {
        std::lock_guard<std::mutex> lock(m_dummyImageMutex);
        for (int y = 0; y < grid; ++y) {
            const float intensityOut = computeDummyIntensity(xSample, y, grid, mid);
            const int v = static_cast<int>(intensityOut * 255.0f);
            const QRgb pixel = qRgba(v, v, v, 255);

            auto* row = reinterpret_cast<QRgb*>(m_dummyImage.scanLine(y));
            row[column] = pixel;

            const int idx = y * 4;
            dst[idx + 0] = static_cast<uchar>(pixel & 0xFF);
            dst[idx + 1] = static_cast<uchar>((pixel >> 8) & 0xFF);
            dst[idx + 2] = static_cast<uchar>((pixel >> 16) & 0xFF);
            dst[idx + 3] = static_cast<uchar>((pixel >> 24) & 0xFF);
        }
    }

    const qint64 columnBytes = columnData.size();

    {
        std::lock_guard<std::mutex> lock(m_dummyUploadMutex);
        m_dummyPendingColumns.push_back({column, std::move(columnData)});
    }

    // Track upload bandwidth
    m_totalBytesUploaded.fetch_add(columnBytes);
    const qint64 elapsedMs = m_uploadTimer.elapsed();
    if (elapsedMs - m_lastBandwidthUpdate >= 1000) {
        const qint64 totalBytes = m_totalBytesUploaded.load();
        const double seconds = elapsedMs / 1000.0;
        const double mbps = (totalBytes / (1024.0 * 1024.0)) / seconds;
        m_uploadBandwidthMBps.store(mbps);
        m_lastBandwidthUpdate = elapsedMs;
    }

    m_dummyWriteColumn = (m_dummyWriteColumn + 1) % grid;
    ++m_dummyTimeIndex;
    m_dummyLastAppendMs = m_dummyClock.elapsed();
    m_dummyLabelDirty = true;

    update();
}

void UnifiedGridRenderer::buildDummyLabelImage() {
    DummyLabelRequest request;
    {
        std::lock_guard<std::mutex> lock(m_dummyLabelRequestMutex);
        request = m_dummyLabelRequest;
    }
    if (!request.valid || request.labelSize.isEmpty()) {
        return;
    }

    const float minCell = std::min(request.cellW, request.cellH);
    const int extraWidth = static_cast<int>(std::ceil(request.cellW));
    QImage labelImage(QSize(request.labelSize.width() + extraWidth, request.labelSize.height()), QImage::Format_ARGB32_Premultiplied);
    labelImage.fill(Qt::transparent);

    QPainter painter(&labelImage);
    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);
    font.setPixelSize(static_cast<int>(std::floor(minCell * 0.45f)));
    painter.setFont(font);
    painter.setPen(QColor(15, 15, 15, 210));

    const int cellsX = static_cast<int>(std::ceil(request.srcRect.width())) + 1;
    const int cellsY = static_cast<int>(std::ceil(request.srcRect.height()));

    {
        std::lock_guard<std::mutex> lock(m_dummyImageMutex);
        for (int j = 0; j < cellsY; ++j) {
            int texY = request.startY + j;
            if (texY < 0) {
                texY = m_dummyImage.height() + (texY % m_dummyImage.height());
            }
            texY = texY % m_dummyImage.height();
            if (texY < 0 || texY >= m_dummyImage.height()) {
                continue;
            }
            for (int i = 0; i < cellsX; ++i) {
                int texX = request.startX + i;
                if (texX < 0) {
                    texX = m_dummyImage.width() + (texX % m_dummyImage.width());
                }
                texX = texX % m_dummyImage.width();
                if (texX < 0 || texX >= m_dummyImage.width()) {
                    continue;
                }
                const QRgb pixel = m_dummyImage.pixel(texX, texY);
                const int value = static_cast<int>((qRed(pixel) / 255.0f) * 20.0f + 0.5f);
                if (value <= 0) {
                    continue;
                }

                const float px = static_cast<float>(i) * request.cellW;
                const float py = static_cast<float>(j) * request.cellH;
                const QRectF cellRect(px, py, request.cellW, request.cellH);
                painter.drawText(cellRect, Qt::AlignCenter, QString::number(value));
            }
        }
    }
    painter.end();

    {
        std::lock_guard<std::mutex> lock(m_dummyLabelMutex);
        m_dummyLabelImage = labelImage;
    }
    m_dummyLabelVersion.fetch_add(1);
}

void UnifiedGridRenderer::onDummyRenderTick() {
    if (!m_useDummyHeatmap || m_dummyGridSize <= 0) {
        return;
    }

    if (m_dummyAppendMs > 0) {
        const qint64 nowMs = m_dummyClock.elapsed();
        const qint64 deltaMs = nowMs - m_dummyLastAppendMs;
        const float frac = std::clamp(static_cast<float>(deltaMs) / static_cast<float>(m_dummyAppendMs), 0.0f, 1.0f);
        const float offset = (static_cast<float>(m_dummyWriteColumn) + frac) / static_cast<float>(m_dummyGridSize);
        m_dummyTimeOffset.store(offset);
    }

    update();
}

// helper methods inlined above; no separate member helpers to avoid private access issues in free functions


// ===== QML DATA API =====
// Methods for data input and manipulation from QML
void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
void UnifiedGridRenderer::togglePerformanceOverlay() { /* No-op: SentinelMonitor removed */ }

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
QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Cells:%1 Size:%2x%3").arg(m_visibleCells ? m_visibleCells->size() : 0).arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString("DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
QString UnifiedGridRenderer::getPerformanceStats() const { return "N/A (SentinelMonitor removed)"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_currentFps.load(); }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

// ===== GPU STATS DEBUG API =====
QString UnifiedGridRenderer::getTextureSize() const {
    if (m_useDummyHeatmap) {
        return QString("%1x%2").arg(m_dummyGridSize).arg(m_dummyGridSize);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureMemory() const {
    if (m_useDummyHeatmap) {
        // RGBA8 = 4 bytes per pixel
        qint64 bytes = static_cast<qint64>(m_dummyGridSize) * m_dummyGridSize * 4;
        double mb = bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    }
    return "N/A";
}

QString UnifiedGridRenderer::getTextureFormat() const {
    if (m_useDummyHeatmap) {
        return "RGBA8";  // Current format
    }
    return "N/A";
}

double UnifiedGridRenderer::getUploadBandwidth() const {
    return m_uploadBandwidthMBps.load();
}

QString UnifiedGridRenderer::getRingCursorInfo() const {
    if (m_useDummyHeatmap) {
        return QString("%1/%2").arg(m_dummyWriteColumn).arg(m_dummyGridSize);
    }
    return "N/A";
}

int UnifiedGridRenderer::getDirtyRegionCount() const {
    if (m_useDummyHeatmap) {
        std::lock_guard<std::mutex> lock(m_dummyUploadMutex);
        return static_cast<int>(m_dummyPendingColumns.size());
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
        m_transformDirty.store(true);  // Mark transform dirty for immediate visual feedback
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
                if (priceDelta != 0.0) {
                    m_viewState->setViewport(
                        m_viewState->getVisibleTimeStart(),
                        m_viewState->getVisibleTimeEnd(),
                        m_viewState->getMinPrice() + priceDelta,
                        m_viewState->getMaxPrice() + priceDelta);
                }
            }
            m_viewState->handlePanEnd(false);
            m_viewState->clearPanVisualOffset();
            m_transformDirty.store(true);
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
        // Request immediate resync of geometry based on new viewport
        if (m_dataProcessor) {
            QMetaObject::invokeMethod(m_dataProcessor.get(), "updateVisibleCells", Qt::QueuedConnection);
        }
        // Keep visual pan until resync arrives to avoid snap-back
        m_panSyncPending = true;
        m_transformDirty.store(true);
        update();
    }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { 
    if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) { 
        m_viewState->handleZoomWithSensitivity(event->angleDelta().y(), event->position(), QSizeF(width(), height())); 
        m_transformDirty.store(true); m_appendPending.store(true); update(); event->accept(); 
    } else event->ignore(); 
}
