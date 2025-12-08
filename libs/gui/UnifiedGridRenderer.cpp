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
#include "../datasources/IGridDataSource.hpp"
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
#include "render/IDataAccessor.hpp"
#include "render/strategies/HeatmapStrategy.hpp"
#include "render/strategies/TradeFlowStrategy.hpp"
#include "render/strategies/TradeBubbleStrategy.hpp"
#include "render/strategies/CandleStrategy.hpp"

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
    
    QMetaObject::invokeMethod(
        m_dataProcessor.get(),
        &DataProcessor::startProcessing,
        Qt::QueuedConnection);
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

    return sceneNode;
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
double UnifiedGridRenderer::getCurrentFPS() const { return 0.0; }
double UnifiedGridRenderer::getAverageRenderTime() const { return 0.0; }
double UnifiedGridRenderer::getCacheHitRate() const { return 0.0; }

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
        m_viewState->handlePanEnd();
        event->accept();
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
