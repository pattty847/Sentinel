#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QDateTime>
#include <algorithm>
#include <cmath>

// New modular architecture includes
#include "render/GridTypes.hpp"
#include "render/GridViewState.hpp"
#include "render/GridSceneNode.hpp" 
#include "render/RenderDiagnostics.hpp"
#include "render/DataProcessor.hpp"
#include "render/IRenderStrategy.hpp"
#include "render/strategies/HeatmapStrategy.hpp"
#include "render/strategies/TradeFlowStrategy.hpp"
#include "render/strategies/CandleStrategy.hpp"

UnifiedGridRenderer::UnifiedGridRenderer(QQuickItem* parent)
    : QQuickItem(parent)
    , m_liquidityEngine(std::make_unique<LiquidityTimeSeriesEngine>(this))
    , m_rootTransformNode(nullptr)
    , m_needsDataRefresh(false)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setFlag(ItemAcceptsInputMethod, true);
    
    // 🐛 FIX: Only accept mouse events on empty areas, not over UI controls
    setAcceptHoverEvents(false);  // Reduce event capture
    
    // Initialize geometry cache
    m_geometryCache.isValid = false;
    m_geometryCache.node = nullptr;
    
    // Order book polling timer removed - now handled by DataProcessor
    
    // 🚀 GEOMETRY CACHE CLEANUP: Periodic cache maintenance every 30 seconds
    QTimer* cacheCleanupTimer = new QTimer(this);
    connect(cacheCleanupTimer, &QTimer::timeout, [this]() {
        // Reset cache occasionally to prevent stale data buildup
        static int cleanupCount = 0;
        sLog_Performance("🧹 Cache cleanup #" << ++cleanupCount << " - geometry cache reset for fresh rendering");
        m_geometryCache.isValid = false;  // Force refresh on next render
        m_geometryDirty.store(true);  // Trigger rebuild
    });
    cacheCleanupTimer->start(30000);  // 30 seconds
    
    // Initialize new modular architecture
    initializeV2Architecture();
    sLog_Init("🚀 UnifiedGridRenderer V2: Modular architecture initialized");
}

UnifiedGridRenderer::~UnifiedGridRenderer() {}

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    // Delegate to DataProcessor - UnifiedGridRenderer is now a slim adapter
    if (m_dataProcessor) {
        m_dataProcessor->onTradeReceived(trade);
    }
}

void UnifiedGridRenderer::onOrderBookUpdated(const OrderBook& book) {
    // Delegate to DataProcessor - UnifiedGridRenderer is now a slim adapter
    if (m_dataProcessor) {
        m_dataProcessor->onOrderBookUpdated(book);
    }
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    // Delegate viewport management to GridViewState
    if (m_viewState) {
        m_viewState->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
    }
    
    m_geometryDirty.store(true);
    update();
    emit viewportChanged(); // 🚀 Notify QML of viewport changes
    
    // Debug logging for viewport changes
    static int viewportChangeCount = 0;
    if (++viewportChangeCount <= 5) {
        sLog_DebugCoords("🎯 UNIFIED RENDERER VIEWPORT #" << viewportChangeCount 
                      << " Time:[" << startTimeMs << "-" << endTimeMs << "]"
                      << " Price:[$" << minPrice << "-$" << maxPrice << "]");
    }
}

void UnifiedGridRenderer::captureOrderBookSnapshot() {
    // Delegated to DataProcessor - this method kept for compatibility
    // but no longer used since DataProcessor handles snapshots
}

void UnifiedGridRenderer::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    if (newGeometry.size() != oldGeometry.size()) {
        // 🚀 THROTTLED GEOMETRY LOGGING: Only log every 5th geometry change to reduce spam
        static int geometryChangeCount = 0;
        if (++geometryChangeCount % 5 == 0) {
            sLog_Render("🎯 UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height() << " [Change #" << geometryChangeCount << "]");
        }
        
        // Force redraw with new geometry
        m_geometryDirty.store(true);
        update();
    }
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    // Route to V2 implementation
    return updatePaintNodeV2(oldNode);
}

void UnifiedGridRenderer::updateVisibleCells() {
    m_visibleCells.clear();
    
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return;
    
    int64_t activeTimeframe = m_currentTimeframe_ms;
    
    // 🚀 OPTIMIZATION 1: Use manual timeframe if set, otherwise auto-suggest
    if (!m_manualTimeframeSet || 
        (m_manualTimeframeTimer.isValid() && m_manualTimeframeTimer.elapsed() > 10000)) {  // 10 second timeout
        
        // Auto-suggest timeframe when no manual override
        int64_t optimalTimeframe = m_liquidityEngine->suggestTimeframe(
            m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(), 
            width() > 0 ? static_cast<int>(width() / 4) : 2000);
        
        if (optimalTimeframe != m_currentTimeframe_ms) {
            m_currentTimeframe_ms = optimalTimeframe;
            activeTimeframe = optimalTimeframe;
            emit timeframeChanged();
            
            sLog_Render("🔄 AUTO-TIMEFRAME UPDATE: " << optimalTimeframe << "ms (viewport-optimized)");
        }
    } else {
        sLog_Render("🎯 MANUAL TIMEFRAME: Using " << m_currentTimeframe_ms << "ms (user-selected)");
    }
    
    // Get liquidity slices for active timeframe within viewport
    auto visibleSlices = m_liquidityEngine->getVisibleSlices(
        activeTimeframe, m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd());
    
    for (const auto* slice : visibleSlices) {
        createCellsFromLiquiditySlice(*slice);
    }
    
    // Debug logging for cell coverage
    static int coverageCount = 0;
    if (++coverageCount <= 5) {
        sLog_Render("🎯 UNIFIED RENDERER COVERAGE #" << coverageCount
                 << " Slices:" << visibleSlices.size()
                 << " TotalCells:" << m_visibleCells.size()
                 << " ActiveTimeframe:" << activeTimeframe << "ms"
                 << " (Manual:" << (m_manualTimeframeSet ? "YES" : "NO") << ")");
    }
}

void UnifiedGridRenderer::updateVolumeProfile() {
    // TODO: Implement volume profile from liquidity time series
    m_volumeProfile.clear();
    
    // For now, create a simple placeholder
    // In a full implementation, this would aggregate volume across time slices
}

void UnifiedGridRenderer::createCellsFromLiquiditySlice(const LiquidityTimeSlice& slice) {
    // Create cells for each price level with liquidity
    for (const auto& [price, metrics] : slice.bidMetrics) {
        double displayValue = slice.getDisplayValue(price, true, static_cast<int>(m_liquidityEngine->getDisplayMode()));
        if (displayValue >= m_minVolumeFilter) {
            createLiquidityCell(slice, price, displayValue, true);
        }
    }
    
    for (const auto& [price, metrics] : slice.askMetrics) {
        double displayValue = slice.getDisplayValue(price, false, static_cast<int>(m_liquidityEngine->getDisplayMode()));
        if (displayValue >= m_minVolumeFilter) {
            createLiquidityCell(slice, price, displayValue, false);
        }
    }
}

void UnifiedGridRenderer::createLiquidityCell(const LiquidityTimeSlice& slice, double price, 
                                            double liquidity, bool isBid) {
    if (!m_viewState || price < m_viewState->getMinPrice() || price > m_viewState->getMaxPrice()) return;
    
    CellInstance cell;
    cell.screenRect = timeSliceToScreenRect(slice, price);
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = calculateIntensity(liquidity);
    cell.color = calculateHeatmapColor(liquidity, isBid, cell.intensity);
    cell.timeSlot = slice.startTime_ms;
    cell.priceLevel = price;
    
    // 🚀 OPTIMIZATION 2: Viewport culling like a videogame (AABB check)
    if (cell.screenRect.right() < 0 || cell.screenRect.left() > width() ||
        cell.screenRect.bottom() < 0 || cell.screenRect.top() > height()) {
        return; // Culled - completely off-screen
    }
    
    // Only add cells with valid screen rectangles
    if (cell.screenRect.width() > 0.1 && cell.screenRect.height() > 0.1) {
        m_visibleCells.push_back(cell);
    }
}

QRectF UnifiedGridRenderer::timeSliceToScreenRect(const LiquidityTimeSlice& slice, double price) const {
    if (!m_viewState || !m_viewState->isTimeWindowValid()) return QRectF();
    
    // Create viewport from current state - delegate to ViewState
    Viewport viewport{
        m_viewState->getVisibleTimeStart(), m_viewState->getVisibleTimeEnd(),
        m_viewState->getMinPrice(), m_viewState->getMaxPrice(),
        width(), height()
    };
    
    // Use unified coordinate system for consistency
    double priceResolution = 1.0;  // $1 price buckets
    QPointF topLeft = CoordinateSystem::worldToScreen(slice.startTime_ms, price + priceResolution/2.0, viewport);
    QPointF bottomRight = CoordinateSystem::worldToScreen(slice.endTime_ms, price - priceResolution/2.0, viewport);
    
    return QRectF(topLeft, bottomRight);
}

// Color calculation methods (delegated to strategies in V2)
QColor UnifiedGridRenderer::calculateHeatmapColor(double liquidity, bool isBid, double intensity) const {
    intensity = std::min(intensity * m_intensityScale, 1.0);
    
    float r, g, b;
    
    if (isBid) {
        // Bid liquidity - Green scale
        r = 0.0f;
        g = 0.3f + intensity * 0.7f;
        b = 0.0f;
    } else {
        // Ask liquidity - Red scale
        r = 0.3f + intensity * 0.7f;
        g = 0.0f;
        b = 0.0f;
    }
    
    // Alpha scales with intensity
    float alpha = 0.1f + intensity * 0.8f;
    
    return QColor::fromRgbF(r, g, b, alpha);
}

double UnifiedGridRenderer::calculateIntensity(double liquidity) const {
    if (liquidity <= 0) return 0.0;
    
    // Simple logarithmic scaling for better visualization
    double intensity = std::log10(1.0 + liquidity * 1000.0) / 4.0;  // Scale to [0,1] range
    return std::min(intensity, 1.0);
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
void UnifiedGridRenderer::addTrade(const Trade& trade) {
    onTradeReceived(trade);
}

void UnifiedGridRenderer::updateOrderBook(const OrderBook& orderBook) {
    onOrderBookUpdated(orderBook);
}

void UnifiedGridRenderer::setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) {
    onViewChanged(timeStart, timeEnd, priceMin, priceMax);
}

void UnifiedGridRenderer::clearData() {
    // Delegate to DataProcessor
    if (m_dataProcessor) {
        m_dataProcessor->clearData();
    }
    
    // Reset liquidity engine
    m_liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>(this);
    
    // Clear rendering data
    m_visibleCells.clear();
    m_volumeProfile.clear();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Init("🎯 UnifiedGridRenderer: Data cleared - delegated to DataProcessor");
}

void UnifiedGridRenderer::setTimeResolution(int resolution_ms) {
    // Note: Manual resolution override implemented via LiquidityTimeSeriesEngine
    sLog_Chart("🎯 Manual time resolution override requested: " << resolution_ms << "ms");
}

void UnifiedGridRenderer::setPriceResolution(double resolution) {
    if (m_liquidityEngine->getPriceResolution() != resolution && resolution > 0) {
        m_liquidityEngine->setPriceResolution(resolution);
        m_geometryDirty.store(true); // Rebuild is necessary with new price buckets
        update();
        sLog_Chart("🎯 Manual price resolution override set to: $" << resolution);
    }
}

void UnifiedGridRenderer::setGridResolution(int timeResMs, double priceRes) {
    setTimeResolution(timeResMs);
    setPriceResolution(priceRes);
    sLog_Chart("Grid resolution set to: " << timeResMs << " ms, $" << priceRes);
}

// Utility to round to a "nice" number
static double getNiceNumber(double raw) {
    if (raw == 0.0) return 0.0;
    double exponent = std::pow(10.0, std::floor(std::log10(raw)));
    double fraction = raw / exponent;
    double niceFraction;
    if (fraction <= 1.0) niceFraction = 1.0;
    else if (fraction <= 2.0) niceFraction = 2.0;
    else if (fraction <= 5.0) niceFraction = 5.0;
    else niceFraction = 10.0;
    return niceFraction * exponent;
}

UnifiedGridRenderer::GridResolution UnifiedGridRenderer::calculateOptimalResolution(qint64 timeSpanMs, double priceSpan, int targetVerticalLines, int targetHorizontalLines) {
    double rawPriceChunk = priceSpan / targetHorizontalLines;
    double nicePriceChunk = getNiceNumber(rawPriceChunk);
    double rawTimeChunkMs = static_cast<double>(timeSpanMs) / targetVerticalLines;
    int niceTimeChunkMs = static_cast<int>(getNiceNumber(rawTimeChunkMs));
    if (nicePriceChunk < 0.00001) nicePriceChunk = 0.00001;
    if (niceTimeChunkMs < 100) niceTimeChunkMs = 100;
    return {niceTimeChunkMs, nicePriceChunk};
}

int UnifiedGridRenderer::getCurrentTimeResolution() const {
    return static_cast<int>(m_currentTimeframe_ms);
}

double UnifiedGridRenderer::getCurrentPriceResolution() const {
    return 1.0;  // $1 default price resolution
}

QString UnifiedGridRenderer::getGridDebugInfo() const {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    QString info = QString("Unified Grid Debug Info:\n"
                          "Time Resolution: %1 ms\n"
                          "Price Resolution: $1.0\n"
                          "Viewport: %2-%3 ms, $%4-$%5\n"
                          "Visible Cells: %6\n"
                          "Screen Size: %7x%8\n"
                          "Renderer Visible: %9\n"
                          "Time Window Valid: %10")
                          .arg(m_currentTimeframe_ms)
                          .arg(getVisibleTimeStart())
                          .arg(getVisibleTimeEnd())
                          .arg(getMinPrice())
                          .arg(getMaxPrice())
                          .arg(m_visibleCells.size())
                          .arg(width())
                          .arg(height())
                          .arg(isVisible() ? "YES" : "NO")
                          .arg(m_viewState && m_viewState->isTimeWindowValid() ? "YES" : "NO");
    
    return info;
}

QString UnifiedGridRenderer::getDetailedGridDebug() const {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    QString debug = QString("🔍 DETAILED UNIFIED RENDERER DEBUG:\n"
                           "Screen: %1x%2\n"
                           "Viewport Time: %3 - %4 (%5ms)\n"
                           "Viewport Price: $%6 - $%7\n"
                           "Liquidity Resolution: %8ms, $1.0\n"
                           "Visible Cells: %9\n"
                           "Has Valid Order Book: %10\n"
                           "Time Window Valid: %11\n")
                           .arg(width()).arg(height())
                           .arg(getVisibleTimeStart()).arg(getVisibleTimeEnd()).arg(getVisibleTimeEnd() - getVisibleTimeStart())
                           .arg(getMinPrice()).arg(getMaxPrice())
                           .arg(m_currentTimeframe_ms)
                           .arg(m_visibleCells.size())
                           .arg(m_dataProcessor && m_dataProcessor->hasValidOrderBook() ? "YES" : "NO")
                           .arg(m_viewState && m_viewState->isTimeWindowValid() ? "YES" : "NO");
    
    // Show first few cells
    int cellsToCheck = std::min(5, (int)m_visibleCells.size());
    for (int i = 0; i < cellsToCheck; ++i) {
        const auto& cell = m_visibleCells[i];
        
        debug += QString("Cell %1: TimeSlot=%2, Price=$%3, Liquidity=%4, IsBid=%5, ScreenRect=(%6,%7 %8x%9)\n")
                .arg(i)
                .arg(cell.timeSlot)
                .arg(cell.priceLevel)
                .arg(cell.liquidity)
                .arg(cell.isBid ? "YES" : "NO")
                .arg(cell.screenRect.x()).arg(cell.screenRect.y())
                .arg(cell.screenRect.width()).arg(cell.screenRect.height());
    }
    
    return debug;
}

void UnifiedGridRenderer::setGridMode(int mode) {
    // 0 = Fine (50ms, $2.50)
    // 1 = Medium (100ms, $5.00)  
    // 2 = Coarse (250ms, $10.00)
    
    int timeRes[] = {50, 100, 250};
    double priceRes[] = {2.5, 5.0, 10.0};
    
    if (mode >= 0 && mode <= 2) {
        setTimeResolution(timeRes[mode]);
        setPriceResolution(priceRes[mode]);
        setTimeframe(timeRes[mode]);  // Also update the timeframe
        
        sLog_Chart("🎯 Grid mode set to:" << (mode == 0 ? "FINE" : mode == 1 ? "MEDIUM" : "COARSE"));
    }
}

void UnifiedGridRenderer::setTimeframe(int timeframe_ms) {
    if (m_currentTimeframe_ms != timeframe_ms) {
        m_currentTimeframe_ms = timeframe_ms;
        
        // 🐛 FIX: Mark as manual timeframe change and start timer
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.start();
        
        // Ensure this timeframe exists in the engine
        m_liquidityEngine->addTimeframe(timeframe_ms);
        
        m_geometryDirty.store(true);
        update();
        
        // 🚀 OPTIMIZATION 4: Emit signal for QML binding updates
        emit timeframeChanged();
        
        sLog_Chart("🎯 MANUAL TIMEFRAME CHANGE: " << timeframe_ms << "ms (auto-suggestion disabled for 10s)");
    }
}

// Pan/Zoom Controls Implementation
void UnifiedGridRenderer::zoomIn() {
    if (m_viewState) {
        m_viewState->handleZoom(1.0, QPointF(width()/2, height()/2));
        m_geometryDirty.store(true);
        update();
        sLog_Chart("🔍 Zoom In");
    }
}

void UnifiedGridRenderer::zoomOut() {
    if (m_viewState) {
        m_viewState->handleZoom(-1.0, QPointF(width()/2, height()/2));
        m_geometryDirty.store(true);
        update();
        sLog_Chart("🔍 Zoom Out");
    }
}

void UnifiedGridRenderer::resetZoom() {
    if (m_viewState) {
        m_viewState->resetZoom();
        m_geometryDirty.store(true);
        update();
        sLog_Chart("🔍 Zoom Reset");
    }
}

void UnifiedGridRenderer::panLeft() {
    if (m_viewState) {
        // Pan left by adjusting time offset
        m_viewState->setViewport(
            m_viewState->getVisibleTimeStart() - 10000,
            m_viewState->getVisibleTimeEnd() - 10000,
            m_viewState->getMinPrice(),
            m_viewState->getMaxPrice()
        );
        m_geometryDirty.store(true);
        update();
        sLog_Chart("👈 Pan Left");
    }
}

void UnifiedGridRenderer::panRight() {
    if (m_viewState) {
        // Pan right by adjusting time offset
        m_viewState->setViewport(
            m_viewState->getVisibleTimeStart() + 10000,
            m_viewState->getVisibleTimeEnd() + 10000,
            m_viewState->getMinPrice(),
            m_viewState->getMaxPrice()
        );
        m_geometryDirty.store(true);
        update();
        sLog_Chart("👉 Pan Right");
    }
}

void UnifiedGridRenderer::panUp() {
    if (m_viewState) {
        // Pan up by adjusting price range
        double priceRange = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
        double panAmount = priceRange * 0.1;
        m_viewState->setViewport(
            m_viewState->getVisibleTimeStart(),
            m_viewState->getVisibleTimeEnd(),
            m_viewState->getMinPrice() + panAmount,
            m_viewState->getMaxPrice() + panAmount
        );
        m_geometryDirty.store(true);
        update();
        sLog_Chart("👆 Pan Up");
    }
}

void UnifiedGridRenderer::panDown() {
    if (m_viewState) {
        // Pan down by adjusting price range
        double priceRange = m_viewState->getMaxPrice() - m_viewState->getMinPrice();
        double panAmount = priceRange * 0.1;
        m_viewState->setViewport(
            m_viewState->getVisibleTimeStart(),
            m_viewState->getVisibleTimeEnd(),
            m_viewState->getMinPrice() - panAmount,
            m_viewState->getMaxPrice() - panAmount
        );
        m_geometryDirty.store(true);
        update();
        sLog_Chart("👇 Pan Down");
    }
}

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    if (m_viewState) {
        m_viewState->enableAutoScroll(enabled);
        m_geometryDirty.store(true);
        update();
        emit autoScrollEnabledChanged();
        sLog_Chart("🕐 Auto-scroll: " << (enabled ? "ENABLED" : "DISABLED"));
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

double UnifiedGridRenderer::getScreenWidth() const {
    return width();
}

double UnifiedGridRenderer::getScreenHeight() const {
    return height();
}

// 🖱️ MOUSE INTERACTION IMPLEMENTATION

void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) {
    // 🎯 MOUSE EVENT FILTERING: Only handle left-button clicks when visible
    if (!isVisible() || event->button() != Qt::LeftButton) {
        event->ignore();  // Let other components handle it
        return;
    }
    
    QPointF pos = event->position();
    
    // 🎯 SPATIAL BOUNDARIES: Define clear UI control areas that QML handles (fixed calculations)
    // Control panel: 150px wide + 10px margin from right edge
    bool inControlPanel = (pos.x() > width() - 160);  // Right side control panel (150px + 10px margin)
    bool inPriceAxis = (pos.x() < 60);  // Left side price axis (60px wide)
    bool inTimeAxis = (pos.y() > height() - 30);  // Bottom time axis (30px high)
    
    if (inControlPanel || inPriceAxis || inTimeAxis) {
        event->ignore();  // Let QML MouseArea components handle these areas
        sLog_Chart("🖱️ Mouse event in UI area - delegating to QML (pos: " << pos.x() << "," << pos.y() << ")");
        return;
    }
    
    // 🎯 DEBUG: Log all mouse events to track conflicts
    sLog_Chart("🖱️ MOUSE PRESS: pos(" << pos.x() << "," << pos.y() << ") size(" << width() << "x" << height() << ")");
    
    // 🎯 CHART INTERACTION AREA: Handle pan/zoom for the main chart area
    if (m_viewState) {
        // Update viewport size for coordinate conversion
        m_viewState->setViewportSize(width(), height());
        
        m_viewState->handlePanStart(pos);
        event->accept();
        sLog_Chart("🖱️ Chart pan started at (" << pos.x() << ", " << pos.y() << ")");
        return;
    }
    
    // Fallback: ignore if no ViewState available
    event->ignore();
}

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) {
    // 🎯 MOUSE MOVE: Only handle if we have an active ViewState
    if (m_viewState) {
        m_viewState->handlePanMove(event->position());
        event->accept(); 
        update(); // Trigger repaint for visual feedback
        sLog_Chart("🖱️ MOUSE MOVE: pos(" << event->position().x() << "," << event->position().y() << ")");
        return;
    }
    
    // Let other components handle if no ViewState
    event->ignore();
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) {
    // Route to V2 ViewState - it now handles coordinate conversion internally
    if (m_viewState) {
        // Update viewport size for coordinate conversion
        m_viewState->setViewportSize(width(), height());
        
        m_viewState->handlePanEnd();
        event->accept();
        update();
        
        m_geometryDirty.store(true);
        emit viewportChanged();
        return;
    }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) {
    // 🎯 WHEEL EVENT FILTERING: Basic validation
    if (!isVisible() || !m_viewState || !m_viewState->isTimeWindowValid() || width() <= 0 || height() <= 0) {
        event->ignore();  // Let other components handle it
        return;
    }
    
    QPointF pos = event->position();
    
    // 🎯 SPATIAL BOUNDARIES: Same as mouse events - respect UI control areas (fixed calculations)
    bool inControlPanel = (pos.x() > width() - 160);  // Right side control panel (150px + 10px margin)
    bool inPriceAxis = (pos.x() < 60);  // Left side price axis (60px wide)
    bool inTimeAxis = (pos.y() > height() - 30);  // Bottom time axis (30px high)
    
    if (inControlPanel || inPriceAxis || inTimeAxis) {
        event->ignore();  // Let QML handle wheel events in UI areas
        sLog_Chart("🖱️ Wheel event in UI area - delegating to QML");
        return;
    }
    
    // 🎯 CHART ZOOM AREA: Handle zoom for the main chart area
    if (m_viewState) {
        // 🐛 FIX: Mouse coordinates are relative to the UnifiedGridRenderer widget (no margins)
        // but the viewport size should match the actual chart rendering area
        QPointF adjustedPos = event->position();  // Mouse position is already relative to our widget
        QSizeF chartSize(width(), height());      // Our widget size IS the chart size
        
        // Use improved zoom method with correct viewport size for proper mouse pointer centering
        m_viewState->handleZoomWithViewport(
            event->angleDelta().y() * 0.001, 
            adjustedPos, 
            chartSize
        );
        m_geometryDirty.store(true);
        update();
        emit autoScrollEnabledChanged(); // Notify UI that auto-scroll is disabled
        event->accept();
        sLog_Chart("🖱️ Chart zoom at (" << adjustedPos.x() << ", " << adjustedPos.y() << ") viewport=" << chartSize.width() << "x" << chartSize.height());
        return;
    }
    
    // Fallback: ignore if no ViewState available
    event->ignore();
}

// 📊 PERFORMANCE MONITORING API

void UnifiedGridRenderer::togglePerformanceOverlay() {
    if (m_diagnostics) {
        m_diagnostics->toggleOverlay();
        sLog_Performance("📊 Performance overlay: " << (m_diagnostics->isOverlayEnabled() ? "ENABLED" : "DISABLED"));
        update();
    }
}

QString UnifiedGridRenderer::getPerformanceStats() const {
    if (m_diagnostics) {
        return m_diagnostics->getPerformanceStats();
    }
    return "Performance monitoring not available";
}

double UnifiedGridRenderer::getCurrentFPS() const {
    if (m_diagnostics) {
        return m_diagnostics->getCurrentFPS();
    }
    return 0.0;
}

double UnifiedGridRenderer::getAverageRenderTime() const {
    if (m_diagnostics) {
        return m_diagnostics->getAverageRenderTime();
    }
    return 0.0;
}

double UnifiedGridRenderer::getCacheHitRate() const {
    if (m_diagnostics) {
        return m_diagnostics->getCacheHitRate();
    }
    return 0.0;
}

// 🚀 NEW MODULAR ARCHITECTURE (V2) IMPLEMENTATION

void UnifiedGridRenderer::initializeV2Architecture() {
    m_viewState = std::make_unique<GridViewState>(this);
    m_diagnostics = std::make_unique<RenderDiagnostics>();
    m_dataProcessor = std::make_unique<DataProcessor>(this);
    
    // Initialize rendering strategies
    m_heatmapStrategy = std::make_unique<HeatmapStrategy>();
    m_tradeFlowStrategy = std::make_unique<TradeFlowStrategy>();
    m_candleStrategy = std::make_unique<CandleStrategy>();
    
    // Wire up V2 components
    m_dataProcessor->setGridViewState(m_viewState.get());
    m_dataProcessor->setLiquidityEngine(m_liquidityEngine.get());
    
    // Connect view state signals to maintain existing API compatibility
    connect(m_viewState.get(), &GridViewState::viewportChanged, 
            this, &UnifiedGridRenderer::viewportChanged);
    connect(m_viewState.get(), &GridViewState::panVisualOffsetChanged,
            this, &UnifiedGridRenderer::panVisualOffsetChanged);
    connect(m_viewState.get(), &GridViewState::autoScrollEnabledChanged,
            this, &UnifiedGridRenderer::autoScrollEnabledChanged);
    
    // Connect data processor signals
    connect(m_dataProcessor.get(), &DataProcessor::dataUpdated,
            this, [this]() {
                m_geometryDirty.store(true);
                update();
            });
    connect(m_dataProcessor.get(), &DataProcessor::viewportInitialized,
            this, &UnifiedGridRenderer::viewportChanged);
    
    // Start data processing
    m_dataProcessor->startProcessing();
            
    sLog_Init("🚀 V2 Architecture: GridViewState + DataProcessor + 3 render strategies initialized");
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

QSGNode* UnifiedGridRenderer::updatePaintNodeV2(QSGNode* oldNode) {
    m_diagnostics->startFrame();
    
    static int paintCallCount = 0;
    if (++paintCallCount <= 10) {
        sLog_Render("🚀 V2 PAINT #" << paintCallCount 
                 << " mode=" << getCurrentStrategy()->getStrategyName()
                 << " size=" << width() << "x" << height() 
                 << " FPS=" << m_diagnostics->getCurrentFPS());
    }
    
    if (width() <= 0 || height() <= 0) {
        return oldNode;
    }
    
    // Use GridSceneNode as root (maintains transform capabilities)
    auto* sceneNode = static_cast<GridSceneNode*>(oldNode);
    bool isNewNode = !sceneNode;
    if (isNewNode) {
        sceneNode = new GridSceneNode();
        m_diagnostics->recordGeometryRebuild();
    }
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // Check if we need to rebuild geometry
        bool needsRebuild = m_geometryDirty.load() || isNewNode;
        
        if (needsRebuild) {
            // Update visible cells using existing logic
            updateVisibleCells();
            
            // Create batch for current strategy
            GridSliceBatch batch;
            batch.cells = m_visibleCells;
            batch.intensityScale = m_intensityScale;
            batch.minVolumeFilter = m_minVolumeFilter;
            batch.maxCells = m_maxCells;
            
            // Update content using selected strategy
            IRenderStrategy* strategy = getCurrentStrategy();
            sceneNode->updateContent(batch, strategy);
            
            // Update volume profile if enabled
            if (m_showVolumeProfile) {
                updateVolumeProfile();
                sceneNode->updateVolumeProfile(m_volumeProfile);
            }
            sceneNode->setShowVolumeProfile(m_showVolumeProfile);
            
            m_geometryDirty.store(false);
            m_diagnostics->recordCacheMiss();
            
            // sLog_Render("🚀 V2 REBUILD: " << batch.cells.size() << " cells, strategy=" 
            //         << strategy->getStrategyName());
        } else {
            m_diagnostics->recordCacheHit();
        }
        
        // Always update transform for smooth interaction
        if (m_viewState) {
            QMatrix4x4 transform = m_viewState->calculateViewportTransform(
                QRectF(0, 0, width(), height()));
            sceneNode->updateTransform(transform);
            m_diagnostics->recordTransformApplied();
        }
    }
    
    m_diagnostics->endFrame();
    return sceneNode;
}

// 🚀 V2 VIEWPORT DELEGATION

qint64 UnifiedGridRenderer::getVisibleTimeStart() const {
    return m_viewState ? m_viewState->getVisibleTimeStart() : 0;
}

qint64 UnifiedGridRenderer::getVisibleTimeEnd() const {
    return m_viewState ? m_viewState->getVisibleTimeEnd() : 0;
}

double UnifiedGridRenderer::getMinPrice() const {
    return m_viewState ? m_viewState->getMinPrice() : 0.0;
}

double UnifiedGridRenderer::getMaxPrice() const {
    return m_viewState ? m_viewState->getMaxPrice() : 0.0;
}

QPointF UnifiedGridRenderer::getPanVisualOffset() const {
    return m_viewState ? m_viewState->getPanVisualOffset() : QPointF(0, 0);
}

