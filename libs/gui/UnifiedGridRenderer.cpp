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
#include "../core/marketdata/cache/DataCache.hpp"
#include "../core/SentinelMonitor.hpp"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QThread>
#include <QMetaObject>
#include <QMetaType>
#include <QDateTime>
// #include <algorithm>
// #include <cmath>

// New modular architecture includes
#include "render/GridTypes.hpp"
#include "render/GridViewState.hpp"
#include "render/GridSceneNode.hpp" 
#include "render/DataProcessor.hpp"
#include "render/IRenderStrategy.hpp"
#include "render/strategies/HeatmapStrategy.hpp"
#include "render/strategies/TradeFlowStrategy.hpp"
#include "render/strategies/CandleStrategy.hpp"

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
    , m_rootTransformNode(nullptr)
    , m_needsDataRefresh(false)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    // 🐛 FIX: Only accept mouse events on empty areas, not over UI controls
    setAcceptHoverEvents(false);  // Reduce event capture
    
    // Initialize new modular architecture
    initializeV2Architecture();
    sLog_App("UnifiedGridRenderer V2: Modular architecture initialized");
}

UnifiedGridRenderer::~UnifiedGridRenderer() {
    // Properly shutdown worker thread
    if (m_dataProcessorThread && m_dataProcessorThread->isRunning()) {
        m_dataProcessorThread->quit();
        m_dataProcessorThread->wait(5000);  // Wait up to 5 seconds
    }
}

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    // Delegate to DataProcessor - UnifiedGridRenderer is now a slim adapter
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), "onTradeReceived", 
                                 Qt::QueuedConnection, Q_ARG(Trade, trade));
    }
}

// Pure delegation to DataProcessor
void UnifiedGridRenderer::onLiveOrderBookUpdated(const QString& productId) {
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), "onLiveOrderBookUpdated", 
                                 Qt::QueuedConnection, Q_ARG(QString, productId));
    }
    
    // Signal that we need a repaint
    m_geometryDirty = true;
    update();
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    // Delegate viewport management to GridViewState
    if (m_viewState) {
        m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
    }
    
    m_geometryDirty.store(true);
    update();
    // GridViewState emits viewportChanged which is connected to us
    
    // Atomic throttling
    sLog_Debug("🎯 UNIFIED RENDERER VIEWPORT Time:[" << startTimeMs << "-" << endTimeMs << "]"
               << " Price:[$" << minPrice << "-$" << maxPrice << "]");
}

void UnifiedGridRenderer::onViewportChanged() {
    if (!m_viewState || !m_dataProcessor) return;
    // Slim adapter: DP + LTSE handle timeframe/LOD; we just mark dirty
    m_geometryDirty.store(true);
    update();
}


void UnifiedGridRenderer::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    if (newGeometry.size() != oldGeometry.size()) {
        // Atomic throttling
        sLog_Render("🎯 UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        // Keep GridViewState in sync with the item size for accurate coord math
        if (m_viewState) {
            m_viewState->setViewportSize(newGeometry.width(), newGeometry.height());
        }

        // Force redraw with new geometry
        m_geometryDirty.store(true);
        update();
    }
}

void UnifiedGridRenderer::updateVisibleCells() {
    // Non-blocking: consume latest snapshot, request async recompute if needed
    if (m_dataProcessor) {
        // Try to grab the latest published cells without blocking the worker
        auto snapshot = m_dataProcessor->getPublishedCellsSnapshot();
        if (snapshot) {
            m_visibleCells.assign(snapshot->begin(), snapshot->end());
        }
    } else {
        m_visibleCells.clear();
    }
    // Avoid writing viewport state from the render thread; size is handled in geometryChanged
}

void UnifiedGridRenderer::updateVolumeProfile() {
    // TODO: Implement volume profile from liquidity time series
    m_volumeProfile.clear();
    
    // For now, create a simple placeholder
    // In a full implementation, this would aggregate volume across time slices
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
        m_geometryDirty.store(true);
        update();
        emit showVolumeProfileChanged();
    }
}

void UnifiedGridRenderer::setIntensityScale(double scale) {
    if (m_intensityScale != scale) {
        m_intensityScale = scale;
        m_geometryDirty.store(true);
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
        m_geometryDirty.store(true);
        update();
        emit minVolumeFilterChanged();
    }
}

// Public API methods
// 🗑️ DELETED: Redundant wrapper methods - use direct calls instead

void UnifiedGridRenderer::clearData() {
    // Delegate to DataProcessor
    if (m_dataProcessor) {
        m_dataProcessor->clearData();
    }
    
    // Clear rendering data
    m_visibleCells.clear();
    m_volumeProfile.clear();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_App("🎯 UnifiedGridRenderer: Data cleared - delegated to DataProcessor");
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_dataProcessor && resolution > 0) {
        m_dataProcessor->setPriceResolution(resolution);
        m_geometryDirty.store(true);
        update();
    }
}

// 🗑️ DELETED: setTimeResolution (empty), setGridResolution (redundant)

// 🗑️ DELETED: Grid calculation utilities, debug methods - use DataProcessor directly

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
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        if (m_dataProcessor) m_dataProcessor->addTimeframe(timeframe_ms);
        m_geometryDirty.store(true);
        update();
        emit timeframeChanged();
    }
}

void UnifiedGridRenderer::zoomIn() { if (m_viewState) { m_viewState->handleZoomWithViewport(0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::zoomOut() { if (m_viewState) { m_viewState->handleZoomWithViewport(-0.1, QPointF(width()/2, height()/2), QSizeF(width(), height())); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::resetZoom() { if (m_viewState) { m_viewState->resetZoom(); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::panLeft() { if (m_viewState) { m_viewState->panLeft(); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::panRight() { if (m_viewState) { m_viewState->panRight(); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::panUp() { if (m_viewState) { m_viewState->panUp(); m_geometryDirty.store(true); update(); } }
void UnifiedGridRenderer::panDown() { if (m_viewState) { m_viewState->panDown(); m_geometryDirty.store(true); update(); } }

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        m_geometryDirty.store(true);
        update();
        emit autoScrollEnabledChanged();
        sLog_Render("🕐 Auto-scroll: " << (enabled ? "ENABLED" : "DISABLED"));
    }
}

// 🎯 COORDINATE SYSTEM INTEGRATION: Expose CoordinateSystem to QML
QPointF UnifiedGridRenderer::worldToScreen(qint64 timestamp_ms, double price) const {
    if (!m_viewState) return QPointF();
    
    Viewport viewport;
    viewport.timeStart_ms = m_viewState->getVisibleTimeStart();
    viewport.timeEnd_ms = m_viewState->getVisibleTimeEnd();
    viewport.priceMin = m_viewState->getMinPrice();
    viewport.priceMax = m_viewState->getMaxPrice();
    viewport.width = width();
    viewport.height = height();
    
    QPointF screenPos = CoordinateSystem::worldToScreen(timestamp_ms, price, viewport);
    
    // 🚀 SMOOTH DRAGGING: Apply visual pan offset for real-time feedback
    QPointF panOffset = getPanVisualOffset();
    screenPos.setX(screenPos.x() + panOffset.x());
    screenPos.setY(screenPos.y() + panOffset.y());
    
    return screenPos;
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
    
    // 🚀 SMOOTH DRAGGING: Remove visual pan offset before world conversion
    QPointF panOffset = getPanVisualOffset();
    QPointF adjustedScreen(screenX - panOffset.x(), screenY - panOffset.y());
    
    return CoordinateSystem::screenToWorld(adjustedScreen, viewport);
}

// 🗑️ DELETED: Redundant getScreenWidth/Height - use width()/height() directly

void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) { 
    if (m_viewState && isVisible() && event->button() == Qt::LeftButton) { 
        m_viewState->setViewportSize(width(), height()); 
        m_viewState->handlePanStart(event->position()); 
        event->accept(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) { 
    if (m_viewState && isVisible() && m_viewState->isTimeWindowValid()) { 
        m_viewState->handleZoomWithSensitivity(event->angleDelta().y(), event->position(), QSizeF(width(), height())); 
        m_geometryDirty.store(true); update(); event->accept(); 
    } else event->ignore(); 
}

void UnifiedGridRenderer::initializeV2Architecture() {
    // Register metatypes for cross-thread signal/slot connections
    qRegisterMetaType<Trade>("Trade");
    
    m_viewState = std::make_unique<GridViewState>(this);
    
    // Create DataProcessor on worker thread for background processing
    m_dataProcessorThread = std::make_unique<QThread>();
    m_dataProcessor = std::make_unique<DataProcessor>();  // No parent - will be moved to thread
    m_dataProcessor->moveToThread(m_dataProcessorThread.get());
    
    // Connect signals with QueuedConnection for thread safety  
    // THROTTLE: Only rebuild geometry every 250ms, not on every data update
    connect(m_dataProcessor.get(), &DataProcessor::dataUpdated, 
            this, [this]() { 
                // Non-blocking refresh: just mark dirty and schedule a frame
                m_geometryDirty.store(true);
                update();
            }, Qt::QueuedConnection);
    connect(m_dataProcessor.get(), &DataProcessor::viewportInitialized,
            this, &UnifiedGridRenderer::viewportChanged, Qt::QueuedConnection);
    
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
    
    m_heatmapStrategy = std::make_unique<HeatmapStrategy>();
    m_tradeFlowStrategy = std::make_unique<TradeFlowStrategy>();
    m_candleStrategy = std::make_unique<CandleStrategy>();
    
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::viewportChanged);
    connect(m_viewState.get(), &GridViewState::viewportChanged, this, &UnifiedGridRenderer::onViewportChanged);
    connect(m_viewState.get(), &GridViewState::panVisualOffsetChanged, this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(m_viewState.get(), &GridViewState::autoScrollEnabledChanged, this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    
    m_dataProcessor->startProcessing();
}

// Dense data access - set cache on both UGR and DataProcessor
void UnifiedGridRenderer::setDataCache(DataCache* cache) {
    m_dataCache = cache;
    
    // Also set the cache on DataProcessor using thread-safe invocation
    if (m_dataProcessor) {
        QMetaObject::invokeMethod(m_dataProcessor.get(), [this, cache]() {
            m_dataProcessor->setDataCache(cache);
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
            
        case RenderMode::VolumeCandles:
            return m_candleStrategy.get();
    }
    
    return m_heatmapStrategy.get(); // Default fallback
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data)  // Not needed for this implementation
    if (width() <= 0 || height() <= 0) {
        // invalid size; skip
        return oldNode;
    }

    QElapsedTimer timer;
    timer.start();

    // reduce noisy frame logs
    if (m_sentinelMonitor) m_sentinelMonitor->startFrame();

    auto* sceneNode = static_cast<GridSceneNode*>(oldNode);
    bool isNewNode = !sceneNode;
    if (isNewNode) {
        sceneNode = new GridSceneNode();
        if (m_sentinelMonitor) m_sentinelMonitor->recordGeometryRebuild();
    }
    
    qint64 cacheUs = 0;
    qint64 contentUs = 0;
    size_t cellsCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        bool needsRebuild = m_geometryDirty.load() || isNewNode;
        
        if (needsRebuild) {
            // needs rebuild
            QElapsedTimer cacheTimer; cacheTimer.start();
            updateVisibleCells(); // now non-blocking; just consumes snapshot
            cacheUs = cacheTimer.nsecsElapsed() / 1000;
            
            // creating batch
            // Render pipeline: passing cells to strategy
            GridSliceBatch batch{m_visibleCells, m_intensityScale, m_minVolumeFilter, m_maxCells};
            
            IRenderStrategy* strategy = getCurrentStrategy();
            // selected strategy
            
            QElapsedTimer contentTimer; contentTimer.start();
            sceneNode->updateContent(batch, strategy);
            contentUs = contentTimer.nsecsElapsed() / 1000;
            if (m_showVolumeProfile) {
                updateVolumeProfile();
                sceneNode->updateVolumeProfile(m_volumeProfile);
            }
            sceneNode->setShowVolumeProfile(m_showVolumeProfile);
            m_geometryDirty.store(false);
            if (m_sentinelMonitor) m_sentinelMonitor->recordCacheMiss();
        } else {
            if (m_sentinelMonitor) m_sentinelMonitor->recordCacheHit();
        }
        
        // Apply visual pan offset as a transform for smooth panning feedback
        QMatrix4x4 transform;
        if (m_viewState) {
            QPointF pan = m_viewState->getPanVisualOffset();
            transform.translate(pan.x(), pan.y());
        }
        sceneNode->updateTransform(transform);
        if (m_sentinelMonitor) m_sentinelMonitor->recordTransformApplied();
        
        cellsCount = m_visibleCells.size();
    }
    
    if (m_sentinelMonitor) {
        m_sentinelMonitor->endFrame();
    }

    const qint64 totalUs = timer.nsecsElapsed() / 1000;
    sLog_RenderN(10, "UGR paint: total=" << totalUs << "µs"
                       << " cache=" << cacheUs << "µs"
                       << " content=" << contentUs << "µs"
                       << " cells=" << static_cast<unsigned long long>(cellsCount));

    return sceneNode;
}

// QML API - Minimal implementations for Q_PROPERTY/Q_INVOKABLE compatibility
void UnifiedGridRenderer::addTrade(const Trade& trade) { onTradeReceived(trade); }
void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) { onViewChanged(timeStart, timeEnd, priceMin, priceMax); }
void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) { setPriceResolution(priceRes); }
int UnifiedGridRenderer::getCurrentTimeResolution() const { return static_cast<int>(m_currentTimeframe_ms); }
double UnifiedGridRenderer::getCurrentPriceResolution() const { return m_dataProcessor ? m_dataProcessor->getPriceResolution() : 1.0; }
QString UnifiedGridRenderer::getGridDebugInfo() const { return QString("Cells:%1 Size:%2x%3").arg(m_visibleCells.size()).arg(width()).arg(height()); }
QString UnifiedGridRenderer::getDetailedGridDebug() const { return getGridDebugInfo() + QString(" DataProcessor:%1").arg(m_dataProcessor ? "YES" : "NO"); }
double UnifiedGridRenderer::getScreenWidth() const { return width(); }
double UnifiedGridRenderer::getScreenHeight() const { return height(); }
qint64 UnifiedGridRenderer::getVisibleTimeStart() const { return m_viewState ? m_viewState->getVisibleTimeStart() : 0; }
qint64 UnifiedGridRenderer::getVisibleTimeEnd() const { return m_viewState ? m_viewState->getVisibleTimeEnd() : 0; }
double UnifiedGridRenderer::getMinPrice() const { return m_viewState ? m_viewState->getMinPrice() : 0.0; }
double UnifiedGridRenderer::getMaxPrice() const { return m_viewState ? m_viewState->getMaxPrice() : 0.0; }
QPointF UnifiedGridRenderer::getPanVisualOffset() const { return m_viewState ? m_viewState->getPanVisualOffset() : QPointF(0, 0); }
void UnifiedGridRenderer::togglePerformanceOverlay() { if (m_sentinelMonitor) m_sentinelMonitor->enablePerformanceOverlay(!m_sentinelMonitor->isOverlayEnabled()); }
QString UnifiedGridRenderer::getPerformanceStats() const { return m_sentinelMonitor ? m_sentinelMonitor->getComprehensiveStats() : "N/A"; }
double UnifiedGridRenderer::getCurrentFPS() const { return m_sentinelMonitor ? m_sentinelMonitor->getCurrentFPS() : 0.0; }
double UnifiedGridRenderer::getAverageRenderTime() const { return m_sentinelMonitor ? m_sentinelMonitor->getAverageFrameTime() : 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return m_sentinelMonitor ? m_sentinelMonitor->getCacheHitRate() : 0.0; }
void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) { if (m_viewState) { m_viewState->handlePanMove(event->position()); event->accept(); update(); } else event->ignore(); }
void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) { if (m_viewState) { m_viewState->setViewportSize(width(), height()); m_viewState->handlePanEnd(); event->accept(); update(); m_geometryDirty.store(true); } }
