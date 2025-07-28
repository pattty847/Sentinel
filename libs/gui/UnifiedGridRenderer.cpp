#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
#include <QDateTime>
#include <algorithm>
#include <cmath>

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
    
    // Setup 100ms order book polling timer for consistent updates
    m_orderBookTimer = new QTimer(this);
    connect(m_orderBookTimer, &QTimer::timeout, this, &UnifiedGridRenderer::captureOrderBookSnapshot);
    m_orderBookTimer->start(100);  // 100ms base resolution
    
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
    
    sLog_Init("🎯 UnifiedGridRenderer: Initialized with LiquidityTimeSeriesEngine");
    sLog_Init("📊 Configuration: 100ms order book snapshots for smooth rendering");
    sLog_Init("⚡ Ready for real-time liquidity data with proper timing");
}

void UnifiedGridRenderer::onTradeReceived(const Trade& trade) {
    if (trade.product_id.empty()) return;
    
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // Initialize viewport from first trade if needed
        if (!m_timeWindowValid) {
            m_visibleTimeStart_ms = timestamp - 30000;  // 30 seconds ago
            m_visibleTimeEnd_ms = timestamp + 30000;    // 30 seconds ahead
            m_minPrice = trade.price - 100.0;
            m_maxPrice = trade.price + 100.0;
            m_timeWindowValid = true;
            
            sLog_Init("🎯 UNIFIED RENDERER VIEWPORT INITIALIZED:");
            sLog_Init("   Time: " << m_visibleTimeStart_ms << " - " << m_visibleTimeEnd_ms);
            sLog_Init("   Price: $" << m_minPrice << " - $" << m_maxPrice);
            sLog_Init("   Based on real trade: $" << trade.price << " at " << timestamp);
        }
        
        // 🚀 SIMPLE: No cache system, always update on new data
        sLog_Render("📊 TRADE UPDATE: Triggering geometry update");
    }
    
    // Debug logging for first few trades
    static int realTradeCount = 0;
    if (++realTradeCount <= 10) {
        sLog_Trades("🎯 UNIFIED RENDERER TRADE #" << realTradeCount << ": $" << trade.price 
                 << " vol:" << trade.size 
                 << " timestamp:" << timestamp);
    }
}

void UnifiedGridRenderer::onOrderBookUpdated(const OrderBook& book) {
    if (book.product_id.empty() || book.bids.empty() || book.asks.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // Store latest order book for snapshots
        m_latestOrderBook = book;
        m_hasValidOrderBook = true;
        
        // Initialize viewport from order book if needed
        if (!m_timeWindowValid) {
            double bestBid = book.bids[0].price;
            double bestAsk = book.asks[0].price;
            double midPrice = (bestBid + bestAsk) / 2.0;
            
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            m_visibleTimeStart_ms = now - 30000;  // 30 seconds ago
            m_visibleTimeEnd_ms = now + 30000;    // 30 seconds ahead
            m_minPrice = midPrice - 100.0;
            m_maxPrice = midPrice + 100.0;
            m_timeWindowValid = true;
            
            sLog_Init("🎯 UNIFIED RENDERER VIEWPORT FROM ORDER BOOK:");
            sLog_Init("   Mid Price: $" << midPrice);
            sLog_Init("   Price Window: $" << m_minPrice << " - $" << m_maxPrice);
        }
    }
    
    // Debug logging for first few order books
    static int orderBookCount = 0;
    if (++orderBookCount <= 10) {
        sLog_Network("🎯 UNIFIED RENDERER ORDER BOOK #" << orderBookCount 
                 << " Bids:" << book.bids.size() << " Asks:" << book.asks.size());
    }
}

void UnifiedGridRenderer::onViewChanged(qint64 startTimeMs, qint64 endTimeMs, 
                                       double minPrice, double maxPrice) {
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        m_visibleTimeStart_ms = startTimeMs;
        m_visibleTimeEnd_ms = endTimeMs;
        m_minPrice = minPrice;
        m_maxPrice = maxPrice;
        m_timeWindowValid = true;
        
        // 🚀 SIMPLE: Always update on viewport changes
        sLog_DebugCoords("🎯 VIEWPORT CHANGED: Will rebuild geometry on next paint");
    }
    
    m_geometryDirty.store(true);
    update();
    
    // Debug logging for viewport changes
    static int viewportChangeCount = 0;
    if (++viewportChangeCount <= 5) {
        sLog_DebugCoords("🎯 UNIFIED RENDERER VIEWPORT #" << viewportChangeCount 
                      << " Time:[" << startTimeMs << "-" << endTimeMs << "]"
                      << " Price:[$" << minPrice << "-$" << maxPrice << "]");
    }
}

void UnifiedGridRenderer::captureOrderBookSnapshot() {
    if (!m_hasValidOrderBook) return;
    
    // Store previous order book hash to detect changes
    static size_t lastOrderBookHash = 0;
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // Calculate simple hash of current order book state
        size_t currentHash = 0;
        for (const auto& bid : m_latestOrderBook.bids) {
            currentHash ^= std::hash<double>{}(bid.price) ^ std::hash<double>{}(bid.size);
        }
        for (const auto& ask : m_latestOrderBook.asks) {
            currentHash ^= std::hash<double>{}(ask.price) ^ std::hash<double>{}(ask.size);
        }
        
        // ALWAYS pass the latest order book to the engine to ensure a continuous time series.
        // This prevents visual gaps in the heatmap when the market is quiet.
        if (m_timeWindowValid) {
            m_liquidityEngine->addOrderBookSnapshot(m_latestOrderBook, m_minPrice, m_maxPrice);
        } else {
            // Fallback to unfiltered if viewport not yet initialized
            m_liquidityEngine->addOrderBookSnapshot(m_latestOrderBook);
        }
        lastOrderBookHash = currentHash; // Keep for potential future debugging
        m_geometryDirty.store(true);
        update();
    }
}

void UnifiedGridRenderer::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    if (newGeometry.size() != oldGeometry.size()) {
        sLog_Render("🎯 UNIFIED RENDERER GEOMETRY CHANGED: " << newGeometry.width() << "x" << newGeometry.height());
        
        // Force redraw with new geometry
        m_geometryDirty.store(true);
        update();
    }
}

QSGNode* UnifiedGridRenderer::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    // 📊 PERFORMANCE MONITORING: Start frame timing
    QElapsedTimer frameTimer;
    frameTimer.start();
    
    // Update frame time history (keep last 60 frames)
    if (m_perfMetrics.frameTimes.size() >= 60) {
        m_perfMetrics.frameTimes.erase(m_perfMetrics.frameTimes.begin());
    }
    if (m_perfMetrics.lastFrameTime_us > 0) {
        m_perfMetrics.frameTimes.push_back(frameTimer.nsecsElapsed() / 1000 - m_perfMetrics.lastFrameTime_us);
    }
    
    // 🔍 DEBUG: Log the first 10 paint calls
    static int paintCallCount = 0;
    if (++paintCallCount <= 10) {
        sLog_Render("🔍 UNIFIED GRID PAINT #" << paintCallCount 
                 << " size=" << width() << "x" << height() 
                 << " visible=" << (isVisible() ? "YES" : "NO")
                 << " geometryDirty=" << (m_geometryDirty.load() ? "YES" : "NO")
                 << " FPS=" << m_perfMetrics.getCurrentFPS());
    }
    
    if (width() <= 0 || height() <= 0) {
        return oldNode;
    }
    
    // 🚀 PERFORMANCE OPTIMIZATION: Use persistent geometry cache + transforms
    auto* rootNode = static_cast<QSGTransformNode*>(oldNode);
    bool isNewNode = !rootNode;
    if (isNewNode) {
        rootNode = new QSGTransformNode();
        m_rootTransformNode = rootNode;
    }

    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // 🚀 PERFORMANCE CRITICAL: Check if we need cache refresh or just transform update
        bool needsRebuild = m_geometryDirty.load() || shouldRefreshCache() || isNewNode;
        
        // 🎯 KEY OPTIMIZATION: Always update transform matrix for smooth interaction
        if (needsRebuild) {
            // This block is expensive. We want to avoid it during a pan.
            // Clear previous children only when rebuilding
            rootNode->removeAllChildNodes();
            
            // Update visible cell data
            updateVisibleCells();
            
            // Create appropriate rendering nodes based on mode
            QSGNode* mainNode = nullptr;
            switch (m_renderMode) {
                case RenderMode::LiquidityHeatmap:
                    mainNode = createHeatmapNode(m_visibleCells);
                    break;
                    
                case RenderMode::TradeFlow:
                    mainNode = createTradeFlowNode(m_visibleCells);
                    break;
                    
                case RenderMode::VolumeCandles:
                    mainNode = createCandleNode(m_visibleCells);
                    break;
                    
                case RenderMode::OrderBookDepth:
                    mainNode = createHeatmapNode(m_visibleCells); // Similar to heatmap
                    break;
            }
            
            if (mainNode) {
                rootNode->appendChildNode(mainNode);
            }
            
            // 🎯 UPDATE GEOMETRY CACHE after successful rebuild
            m_geometryCache.isValid = true;
            m_geometryCache.cacheTimeStart_ms = m_visibleTimeStart_ms;
            m_geometryCache.cacheTimeEnd_ms = m_visibleTimeEnd_ms;
            m_geometryCache.cacheMinPrice = m_minPrice;
            m_geometryCache.cacheMaxPrice = m_maxPrice;
            
            sLog_Render("🎯 OPTIMIZED RENDER: " << m_visibleCells.size() << " cells rendered (rebuild) + cache updated");
            
            // Add volume profile if enabled (TODO: could also be cached)
            if (m_showVolumeProfile) {
                updateVolumeProfile();
                QSGNode* profileNode = createVolumeProfileNode(m_volumeProfile);
                if (profileNode) {
                    rootNode->appendChildNode(profileNode);
                }
            }
        } else {
            sLog_Render("🎯 TRANSFORM-ONLY UPDATE: Smooth pan/zoom without geometry rebuild");
        }

        // 🚀 ALWAYS apply transform. During a pan, this is the ONLY thing that happens.
        QMatrix4x4 transform = calculateViewportTransform();
        // Apply the temporary visual pan offset
        transform.translate(m_panVisualOffset.x(), m_panVisualOffset.y());
        rootNode->setMatrix(transform);
    }
    
    m_geometryDirty.store(false);
    m_needsDataRefresh = false;
    
    // 📊 PERFORMANCE MONITORING: End frame timing
    m_perfMetrics.renderTime_us = frameTimer.nsecsElapsed() / 1000;
    m_perfMetrics.lastFrameTime_us = frameTimer.nsecsElapsed() / 1000;
    
    // Log performance summary every 100 frames
    static int frameCount = 0;
    if (++frameCount % 100 == 0) {
        sLog_Performance("📊 PERF SUMMARY (100 frames): FPS=" << m_perfMetrics.getCurrentFPS() 
                      << " AvgRender=" << m_perfMetrics.getAverageRenderTime_ms() << "ms"
                      << " CacheHitRate=" << m_perfMetrics.getCacheHitRate() << "%"
                      << " CachedCells=" << m_perfMetrics.cachedCellCount);
    }
    
    sLog_Render("🎯 UNIFIED GRID RENDER COMPLETE (" << m_perfMetrics.renderTime_us / 1000.0 << "ms)");
    
    return rootNode;
}

void UnifiedGridRenderer::updateVisibleCells() {
    m_visibleCells.clear();
    
    if (!m_timeWindowValid) return;
    
    int64_t activeTimeframe = m_currentTimeframe_ms;
    
    // 🚀 OPTIMIZATION 1: Use manual timeframe if set, otherwise auto-suggest
    if (!m_manualTimeframeSet || 
        (m_manualTimeframeTimer.isValid() && m_manualTimeframeTimer.elapsed() > 10000)) {  // 10 second timeout
        
        // Auto-suggest timeframe when no manual override
        int64_t optimalTimeframe = m_liquidityEngine->suggestTimeframe(
            m_visibleTimeStart_ms, m_visibleTimeEnd_ms, width() > 0 ? static_cast<int>(width() / 4) : 2000);
        
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
        activeTimeframe, m_visibleTimeStart_ms, m_visibleTimeEnd_ms);
    
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
    if (price < m_minPrice || price > m_maxPrice) return;
    
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
    if (!m_timeWindowValid) return QRectF();
    
    // Create viewport from current state
    Viewport viewport{
        m_visibleTimeStart_ms, m_visibleTimeEnd_ms,
        m_minPrice, m_maxPrice,
        width(), height()
    };
    
    // Use unified coordinate system for consistency
    double priceResolution = 1.0;  // $1 price buckets
    QPointF topLeft = CoordinateSystem::worldToScreen(slice.startTime_ms, price + priceResolution/2.0, viewport);
    QPointF bottomRight = CoordinateSystem::worldToScreen(slice.endTime_ms, price - priceResolution/2.0, viewport);
    
    return QRectF(topLeft, bottomRight);
}

QSGNode* UnifiedGridRenderer::createHeatmapNode(const std::vector<CellInstance>& cells) {
    if (cells.empty()) return nullptr;
    
    auto* node = new QSGGeometryNode();
    
    // Create geometry for quads (6 vertices per cell)
    int vertexCount = cells.size() * 6;
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        const QRectF& rect = cell.screenRect;
        
        // Convert color to vertex format
        uchar r = static_cast<uchar>(cell.color.red());
        uchar g = static_cast<uchar>(cell.color.green());
        uchar b = static_cast<uchar>(cell.color.blue());
        uchar a = static_cast<uchar>(cell.color.alpha());
        
        size_t baseIdx = i * 6;
        
        // Two triangles forming a quad
        // Triangle 1: top-left, top-right, bottom-left
        vertices[baseIdx + 0].set(rect.left(), rect.top(), r, g, b, a);
        vertices[baseIdx + 1].set(rect.right(), rect.top(), r, g, b, a);
        vertices[baseIdx + 2].set(rect.left(), rect.bottom(), r, g, b, a);
        
        // Triangle 2: top-right, bottom-right, bottom-left
        vertices[baseIdx + 3].set(rect.right(), rect.top(), r, g, b, a);
        vertices[baseIdx + 4].set(rect.right(), rect.bottom(), r, g, b, a);
        vertices[baseIdx + 5].set(rect.left(), rect.bottom(), r, g, b, a);
    }
    
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Use vertex color material for per-vertex colors
    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Track bandwidth usage
    m_bytesUploadedThisFrame += vertexCount * sizeof(QSGGeometry::ColoredPoint2D);
    
    return node;
}

QSGNode* UnifiedGridRenderer::createTradeFlowNode(const std::vector<CellInstance>& cells) {
    if (cells.empty()) return nullptr;
    
    auto* node = new QSGGeometryNode();
    
    // Create geometry for circles (similar to trade dots but grid-based)
    int vertexCount = cells.size() * 6; // 6 vertices per circle (2 triangles)
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        const QRectF& rect = cell.screenRect;
        
        // Convert color to vertex format
        uchar r = static_cast<uchar>(cell.color.red());
        uchar g = static_cast<uchar>(cell.color.green());
        uchar b = static_cast<uchar>(cell.color.blue());
        uchar a = static_cast<uchar>(cell.color.alpha());
        
        // Calculate circle radius based on volume
        float radius = std::max(2.0f, static_cast<float>(cell.intensity * 10.0f));
        QPointF center(rect.center());
        
        size_t baseIdx = i * 6;
        
        // Create a simple circle approximation with 2 triangles
        // Triangle 1: center, top-left, top-right
        vertices[baseIdx + 0].set(center.x(), center.y(), r, g, b, a);
        vertices[baseIdx + 1].set(center.x() - radius, center.y() - radius, r, g, b, a);
        vertices[baseIdx + 2].set(center.x() + radius, center.y() - radius, r, g, b, a);
        
        // Triangle 2: center, bottom-left, bottom-right
        vertices[baseIdx + 3].set(center.x(), center.y(), r, g, b, a);
        vertices[baseIdx + 4].set(center.x() - radius, center.y() + radius, r, g, b, a);
        vertices[baseIdx + 5].set(center.x() + radius, center.y() + radius, r, g, b, a);
    }
    
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Use vertex color material for per-vertex colors
    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Track bandwidth usage
    m_bytesUploadedThisFrame += vertexCount * sizeof(QSGGeometry::ColoredPoint2D);
    
    return node;
}

QSGNode* UnifiedGridRenderer::createCandleNode(const std::vector<CellInstance>& cells) {
    if (cells.empty()) return nullptr;
    
    auto* node = new QSGGeometryNode();
    
    // Create simple volume bars instead of OHLC candles
    int vertexCount = cells.size() * 6; // 6 vertices per bar (2 triangles)
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& cell = cells[i];
        const QRectF& rect = cell.screenRect;
        
        // Convert color to vertex format
        uchar r = static_cast<uchar>(cell.color.red());
        uchar g = static_cast<uchar>(cell.color.green());
        uchar b = static_cast<uchar>(cell.color.blue());
        uchar a = static_cast<uchar>(cell.color.alpha());
        
        // Create a simple volume bar
        float barWidth = static_cast<float>(rect.width() * 0.8); // 80% of cell width
        float barHeight = static_cast<float>(rect.height() * cell.intensity);
        float left = rect.center().x() - barWidth / 2;
        float right = rect.center().x() + barWidth / 2;
        float top = rect.center().y() - barHeight / 2;
        float bottom = rect.center().y() + barHeight / 2;
        
        size_t baseIdx = i * 6;
        
        // Bar quad (2 triangles = 6 vertices)
        vertices[baseIdx + 0].set(left, bottom, r, g, b, a);
        vertices[baseIdx + 1].set(right, bottom, r, g, b, a);
        vertices[baseIdx + 2].set(right, top, r, g, b, a);
        vertices[baseIdx + 3].set(right, top, r, g, b, a);
        vertices[baseIdx + 4].set(left, top, r, g, b, a);
        vertices[baseIdx + 5].set(left, bottom, r, g, b, a);
    }
    
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    auto* material = new QSGVertexColorMaterial();
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    m_bytesUploadedThisFrame += vertexCount * sizeof(QSGGeometry::ColoredPoint2D);
    
    return node;
}

QSGNode* UnifiedGridRenderer::createVolumeProfileNode(const std::vector<std::pair<double, double>>& profile) {
    if (profile.empty()) return nullptr;
    
    auto* node = new QSGGeometryNode();
    
    // Create horizontal bars for volume profile
    int vertexCount = profile.size() * 6; // 6 vertices per bar
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    
    // Find max volume for scaling
    double maxVolume = 0.0;
    for (const auto& [price, volume] : profile) {
        maxVolume = std::max(maxVolume, volume);
    }
    
    if (maxVolume <= 0) return nullptr;
    
    double maxBarWidth = width() * 0.2; // Max 20% of screen width
    
    for (size_t i = 0; i < profile.size(); ++i) {
        const auto& [price, volume] = profile[i];
        
        // Map price to screen Y coordinate
        double priceRange = m_maxPrice - m_minPrice;
        double priceRatio = (price - m_minPrice) / priceRange;
        float y = static_cast<float>((1.0 - priceRatio) * height());
        
        // Calculate bar dimensions
        float barHeight = static_cast<float>(height() / profile.size());
        float barWidth = static_cast<float>((volume / maxVolume) * maxBarWidth);
        
        float left = width() - barWidth;
        float right = width();
        float top = y - barHeight / 2;
        float bottom = y + barHeight / 2;
        
        // Volume profile color (semi-transparent white)
        uchar r = 255, g = 255, b = 255, a = 128;
        
        size_t baseIdx = i * 6;
        
        // Bar quad
        vertices[baseIdx + 0].set(left, bottom, r, g, b, a);
        vertices[baseIdx + 1].set(right, bottom, r, g, b, a);
        vertices[baseIdx + 2].set(right, top, r, g, b, a);
        vertices[baseIdx + 3].set(right, top, r, g, b, a);
        vertices[baseIdx + 4].set(left, top, r, g, b, a);
        vertices[baseIdx + 5].set(left, bottom, r, g, b, a);
    }
    
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    auto* material = new QSGVertexColorMaterial();
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    return node;
}

// Color calculation methods
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

QColor UnifiedGridRenderer::calculateTradeFlowColor(double liquidity, bool isBid, double intensity) const {
    intensity = std::min(intensity * m_intensityScale, 1.0);
    
    if (isBid) {
        // Bid-heavy: Green
        return QColor::fromRgbF(0.0f, 0.8f, 0.0f, 0.6f + intensity * 0.4f);
    } else {
        // Ask-heavy: Red
        return QColor::fromRgbF(0.8f, 0.0f, 0.0f, 0.6f + intensity * 0.4f);
    }
}

QColor UnifiedGridRenderer::calculateCandleColor(double liquidity, bool isBid, double intensity) const {
    if (liquidity <= 0) return QColor(128, 128, 128, 128);
    
    // Green for high volume, red for low volume
    if (intensity > 0.5) {
        return QColor(0, 255, 0, 180); // High volume: Green
    } else {
        return QColor(255, 0, 0, 180); // Low volume: Red
    }
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
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // Reset liquidity engine
    m_liquidityEngine = std::make_unique<LiquidityTimeSeriesEngine>(this);
    
    // Clear rendering data
    m_visibleCells.clear();
    m_volumeProfile.clear();
    
    // 🚀 SIMPLE: Clear rendering data only
    sLog_Init("🎯 CLEAR DATA: Cleared rendering data");
    
    // Reset viewport state
    m_timeWindowValid = false;
    m_hasValidOrderBook = false;
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Init("🎯 UnifiedGridRenderer: Data cleared");
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
                          .arg(m_visibleTimeStart_ms)
                          .arg(m_visibleTimeEnd_ms)
                          .arg(m_minPrice)
                          .arg(m_maxPrice)
                          .arg(m_visibleCells.size())
                          .arg(width())
                          .arg(height())
                          .arg(isVisible() ? "YES" : "NO")
                          .arg(m_timeWindowValid ? "YES" : "NO");
    
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
                           .arg(m_visibleTimeStart_ms).arg(m_visibleTimeEnd_ms).arg(m_visibleTimeEnd_ms - m_visibleTimeStart_ms)
                           .arg(m_minPrice).arg(m_maxPrice)
                           .arg(m_currentTimeframe_ms)
                           .arg(m_visibleCells.size())
                           .arg(m_hasValidOrderBook ? "YES" : "NO")
                           .arg(m_timeWindowValid ? "YES" : "NO");
    
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

// 🚀 PERFORMANCE OPTIMIZATION METHODS

void UnifiedGridRenderer::updateCachedGeometry() {
    // 🚀 BALANCED: Load data for wider area but not too much
    int64_t visibleTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double visiblePriceRange = m_maxPrice - m_minPrice;
    
    // Use 2x buffer - reasonable for smooth panning without too much memory
    int64_t cacheTimeBuffer = visibleTimeRange * 2; 
    double cachePriceBuffer = visiblePriceRange * 2.0;
    
    int64_t cacheTimeStart = m_visibleTimeStart_ms - cacheTimeBuffer;
    int64_t cacheTimeEnd = m_visibleTimeEnd_ms + cacheTimeBuffer;
    double cacheMinPrice = m_minPrice - cachePriceBuffer;
    double cacheMaxPrice = m_maxPrice + cachePriceBuffer;
    
    // Get liquidity slices for the expanded cache area
    auto cacheSlices = m_liquidityEngine->getVisibleSlices(
        m_currentTimeframe_ms, cacheTimeStart, cacheTimeEnd);
    
    m_geometryCache.cachedCells.clear();
    
    for (const auto* slice : cacheSlices) {
        // Process bid metrics
        for (const auto& [price, metrics] : slice->bidMetrics) {
            if (price >= cacheMinPrice && price <= cacheMaxPrice) {
                double displayValue = slice->getDisplayValue(price, true, 0);
                if (displayValue > 0.0) {
                    createCachedLiquidityCell(*slice, price, displayValue, true);
                }
            }
        }
        
        // Process ask metrics
        for (const auto& [price, metrics] : slice->askMetrics) {
            if (price >= cacheMinPrice && price <= cacheMaxPrice) {
                double displayValue = slice->getDisplayValue(price, false, 0);
                if (displayValue > 0.0) {
                    createCachedLiquidityCell(*slice, price, displayValue, false);
                }
            }
        }
    }
    
    sLog_Render("📊 CACHE UPDATE: Loaded " << m_geometryCache.cachedCells.size() 
             << " cells for expanded area " << cacheTimeStart << "-" << cacheTimeEnd
             << " price $" << cacheMinPrice << "-$" << cacheMaxPrice);
    
    // 🚀 OPTIMIZATION 3: Mark cache as valid and store bounds (fixes rebuild spam!)
    m_geometryCache.isValid = true;
    m_geometryCache.cacheTimeStart_ms = cacheTimeStart;
    m_geometryCache.cacheTimeEnd_ms = cacheTimeEnd;
    m_geometryCache.cacheMinPrice = cacheMinPrice;
    m_geometryCache.cacheMaxPrice = cacheMaxPrice;
}

bool UnifiedGridRenderer::shouldRefreshCache() const {
    if (!m_geometryCache.isValid) return true;
    
    // 🔧 DEBUG: Log cache bounds checking
    static int checkCount = 0;
    bool shouldLog = (++checkCount <= 10) || (checkCount % 50 == 0);
    
    if (shouldLog) {
        sLog_Render("🔍 CACHE CHECK #" << checkCount << ":");
        sLog_Render("   Current viewport: time[" << m_visibleTimeStart_ms << "-" << m_visibleTimeEnd_ms << "] price[$" << m_minPrice << "-$" << m_maxPrice << "]");
        sLog_Render("   Cached bounds: time[" << m_geometryCache.cacheTimeStart_ms << "-" << m_geometryCache.cacheTimeEnd_ms << "] price[$" << m_geometryCache.cacheMinPrice << "-$" << m_geometryCache.cacheMaxPrice << "]");
    }
    
    // 🚀 SIMPLE AND WORKING: Only refresh when viewport moves significantly outside cached area
    int64_t cacheTimeRange = m_geometryCache.cacheTimeEnd_ms - m_geometryCache.cacheTimeStart_ms;
    double cachePriceRange = m_geometryCache.cacheMaxPrice - m_geometryCache.cacheMinPrice;
    
    // Refresh cache if viewport moved more than 25% outside cached bounds
    int64_t timeBuffer = cacheTimeRange * 0.25; // 25% buffer
    double priceBuffer = cachePriceRange * 0.25; // 25% buffer
    
    bool timeOutOfBounds = (m_visibleTimeStart_ms < m_geometryCache.cacheTimeStart_ms + timeBuffer) ||
                          (m_visibleTimeEnd_ms > m_geometryCache.cacheTimeEnd_ms - timeBuffer);
    
    bool priceOutOfBounds = (m_minPrice < m_geometryCache.cacheMinPrice + priceBuffer) ||
                           (m_maxPrice > m_geometryCache.cacheMaxPrice - priceBuffer);
    
    bool shouldRefresh = timeOutOfBounds || priceOutOfBounds;
    
    if (shouldLog) {
        sLog_Render("   timeOutOfBounds=" << (timeOutOfBounds ? "YES" : "NO")
                 << " priceOutOfBounds=" << (priceOutOfBounds ? "YES" : "NO")
                 << " -> REFRESH=" << (shouldRefresh ? "YES" : "NO"));
    }
    
    return shouldRefresh;
}

QMatrix4x4 UnifiedGridRenderer::calculateViewportTransform() const {
    if (!m_geometryCache.isValid) return QMatrix4x4();
    
    // Calculate transform to map from cache coordinates to current viewport
    QMatrix4x4 transform;
    
    // Calculate scale factors
    double cacheTimeRange = m_geometryCache.cacheTimeEnd_ms - m_geometryCache.cacheTimeStart_ms;
    double cachePriceRange = m_geometryCache.cacheMaxPrice - m_geometryCache.cacheMinPrice;
    double viewTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double viewPriceRange = m_maxPrice - m_minPrice;
    
    if (cacheTimeRange <= 0 || cachePriceRange <= 0 || viewTimeRange <= 0 || viewPriceRange <= 0) {
        return QMatrix4x4();
    }
    
    float scaleX = static_cast<float>(cacheTimeRange / viewTimeRange);
    float scaleY = static_cast<float>(cachePriceRange / viewPriceRange);
    
    // Calculate translation offsets
    double timeOffset = (m_visibleTimeStart_ms - m_geometryCache.cacheTimeStart_ms) / cacheTimeRange;
    double priceOffset = (m_minPrice - m_geometryCache.cacheMinPrice) / cachePriceRange;
    
    float translateX = static_cast<float>(-timeOffset * width() * scaleX);
    float translateY = static_cast<float>(priceOffset * height() * scaleY);
    
    // Apply transformations
    transform.translate(translateX, translateY);
    transform.scale(scaleX, scaleY);
    
    return transform;
}

void UnifiedGridRenderer::createCachedLiquidityCell(const LiquidityTimeSlice& slice, 
                                                   double price, double liquidity, bool isBid) {
    CellInstance cell;
    
    // Use cache coordinates instead of screen coordinates
    cell.screenRect = timeSliceToCacheRect(slice, price);
    cell.liquidity = liquidity;
    cell.isBid = isBid;
    cell.intensity = calculateIntensity(liquidity);
    cell.color = calculateHeatmapColor(liquidity, isBid, cell.intensity);
    cell.timeSlot = slice.startTime_ms;
    cell.priceLevel = price;
    
    // Only add cells with valid rectangles
    if (cell.screenRect.width() > 0.1 && cell.screenRect.height() > 0.1) {
        m_geometryCache.cachedCells.push_back(cell);
    }
}

QRectF UnifiedGridRenderer::timeSliceToCacheRect(const LiquidityTimeSlice& slice, double price) const {
    if (!m_geometryCache.isValid && !m_timeWindowValid) return QRectF();
    
    // Determine cache bounds
    int64_t cacheTimeStart = (m_geometryCache.isValid) ? m_geometryCache.cacheTimeStart_ms : m_visibleTimeStart_ms;
    int64_t cacheTimeEnd = (m_geometryCache.isValid) ? m_geometryCache.cacheTimeEnd_ms : m_visibleTimeEnd_ms;
    double cacheMinPrice = (m_geometryCache.isValid) ? m_geometryCache.cacheMinPrice : m_minPrice;
    double cacheMaxPrice = (m_geometryCache.isValid) ? m_geometryCache.cacheMaxPrice : m_maxPrice;
    
    // Create viewport from cache bounds
    Viewport cacheViewport{
        cacheTimeStart, cacheTimeEnd,
        cacheMinPrice, cacheMaxPrice,
        width(), height()
    };
    
    // Use unified coordinate system for consistency
    double priceResolution = 1.0;  // $1 price buckets
    QPointF topLeft = CoordinateSystem::worldToScreen(slice.startTime_ms, price + priceResolution/2.0, cacheViewport);
    QPointF bottomRight = CoordinateSystem::worldToScreen(slice.endTime_ms, price - priceResolution/2.0, cacheViewport);
    
    return QRectF(topLeft, bottomRight);
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
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_zoomFactor *= 1.5;
    m_zoomFactor = std::min(m_zoomFactor, 50.0);  // Max zoom
    
    // Update viewport with zoom
    updateViewportFromZoom();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("🔍 Zoom In - Factor: " << m_zoomFactor);
}

void UnifiedGridRenderer::zoomOut() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_zoomFactor /= 1.5;
    m_zoomFactor = std::max(m_zoomFactor, 0.1);  // Min zoom
    
    // Update viewport with zoom
    updateViewportFromZoom();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("🔍 Zoom Out - Factor: " << m_zoomFactor);
}

void UnifiedGridRenderer::resetZoom() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_zoomFactor = 1.0;
    m_panOffsetTime_ms = 0.0;
    m_panOffsetPrice = 0.0;
    m_autoScrollEnabled = true;
    
    // Reset to initial viewport
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    m_visibleTimeStart_ms = now - 30000;  // 30 seconds ago
    m_visibleTimeEnd_ms = now + 30000;    // 30 seconds ahead
    
    if (m_hasValidOrderBook && !m_latestOrderBook.bids.empty() && !m_latestOrderBook.asks.empty()) {
        double bestBid = m_latestOrderBook.bids[0].price;
        double bestAsk = m_latestOrderBook.asks[0].price;
        double midPrice = (bestBid + bestAsk) / 2.0;
        m_minPrice = midPrice - 100.0;
        m_maxPrice = midPrice + 100.0;
    }
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("🔍 Zoom Reset");
}

void UnifiedGridRenderer::panLeft() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_autoScrollEnabled = false;  // Disable auto-scroll when manually panning
    
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1;  // Pan 10% of visible range
    
    m_panOffsetTime_ms -= panAmount;
    updateViewportFromPan();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("👈 Pan Left - Offset: " << m_panOffsetTime_ms);
}

void UnifiedGridRenderer::panRight() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_autoScrollEnabled = false;  // Disable auto-scroll when manually panning
    
    int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    int64_t panAmount = timeRange * 0.1;  // Pan 10% of visible range
    
    m_panOffsetTime_ms += panAmount;
    updateViewportFromPan();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("👉 Pan Right - Offset: " << m_panOffsetTime_ms);
}

void UnifiedGridRenderer::panUp() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1;  // Pan 10% of visible range
    
    m_panOffsetPrice += panAmount;
    updateViewportFromPan();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("👆 Pan Up - Offset: " << m_panOffsetPrice);
}

void UnifiedGridRenderer::panDown() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    double priceRange = m_maxPrice - m_minPrice;
    double panAmount = priceRange * 0.1;  // Pan 10% of visible range
    
    m_panOffsetPrice -= panAmount;
    updateViewportFromPan();
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("👇 Pan Down - Offset: " << m_panOffsetPrice);
}

void UnifiedGridRenderer::enableAutoScroll(bool enabled) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_autoScrollEnabled = enabled;
    
    if (enabled) {
        // Reset to current time when enabling auto-scroll
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        m_visibleTimeStart_ms = now - timeRange / 2;
        m_visibleTimeEnd_ms = now + timeRange / 2;
        m_panOffsetTime_ms = 0.0;
    }
    
    m_geometryDirty.store(true);
    update();
    emit autoScrollEnabledChanged();
    
    sLog_Chart("🕐 Auto-scroll: " << (enabled ? "ENABLED" : "DISABLED"));
}

// Viewport management helpers
void UnifiedGridRenderer::updateViewportFromZoom() {
    if (!m_timeWindowValid) return;
    
    // Calculate center point
    int64_t centerTime = (m_visibleTimeStart_ms + m_visibleTimeEnd_ms) / 2 + static_cast<int64_t>(m_panOffsetTime_ms);
    double centerPrice = (m_minPrice + m_maxPrice) / 2.0 + m_panOffsetPrice;
    
    // Calculate new ranges based on zoom factor
    int64_t baseTimeRange = 60000;  // 60 second base range
    int64_t newTimeRange = static_cast<int64_t>(baseTimeRange / m_zoomFactor);
    
    double basePriceRange = 200.0;  // $200 base range
    double newPriceRange = basePriceRange / m_zoomFactor;
    
    // Apply new viewport
    m_visibleTimeStart_ms = centerTime - newTimeRange / 2;
    m_visibleTimeEnd_ms = centerTime + newTimeRange / 2;
    m_minPrice = centerPrice - newPriceRange / 2.0;
    m_maxPrice = centerPrice + newPriceRange / 2.0;
}

void UnifiedGridRenderer::updateViewportFromPan() {
    if (!m_timeWindowValid) return;
    
    // Calculate base viewport
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    int64_t baseTimeRange = static_cast<int64_t>(60000 / m_zoomFactor);  // Zoom-adjusted range
    double basePriceRange = 200.0 / m_zoomFactor;
    
    // Apply pan offsets
    int64_t centerTime = now + static_cast<int64_t>(m_panOffsetTime_ms);
    double centerPrice = (m_hasValidOrderBook && !m_latestOrderBook.bids.empty() && !m_latestOrderBook.asks.empty()) ?
        (m_latestOrderBook.bids[0].price + m_latestOrderBook.asks[0].price) / 2.0 : 50000.0;
    centerPrice += m_panOffsetPrice;
    
    m_visibleTimeStart_ms = centerTime - baseTimeRange / 2;
    m_visibleTimeEnd_ms = centerTime + baseTimeRange / 2;
    m_minPrice = centerPrice - basePriceRange / 2.0;
    m_maxPrice = centerPrice + basePriceRange / 2.0;
}

// 🖱️ MOUSE INTERACTION IMPLEMENTATION

void UnifiedGridRenderer::mousePressEvent(QMouseEvent* event) {
    // 🐛 FIX: Only handle mouse events when visible and in grid mode
    if (!isVisible() || event->button() != Qt::LeftButton) {
        event->ignore();  // Let other components handle it
        return;
    }
    
    // 🐛 FIX: Check if click is over UI control areas (exclude top-right corner for controls)
    QPointF pos = event->position();
    if (pos.x() > width() * 0.7 && pos.y() < height() * 0.4) {
        event->ignore();  // Let UI controls handle it
        return;
    }
    
    m_interactionTimer.start();
    m_isDragging = true;
    m_lastMousePos = pos;
    m_initialMousePos = pos;  // Store initial click position

    // Reset visual pan offset at the start of a drag
    m_panVisualOffset = {0.0, 0.0};
    
    // 🚀 USER CONTROL: Disable auto-scroll when user starts dragging
    m_autoScrollEnabled = false;
    
    m_geometryDirty.store(true);
    update();
    
    sLog_Chart("🖱️ Mouse drag started - auto-scroll disabled");
    event->accept();
}

void UnifiedGridRenderer::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && m_timeWindowValid) {
        // 🚀 FAST PAN LOGIC
        QPointF currentPos = event->position();
        QPointF delta = currentPos - m_lastMousePos;

        // Accumulate the visual offset
        m_panVisualOffset += delta;
        m_lastMousePos = currentPos;

        // Trigger a repaint to apply the new visual transform.
        // Do NOT set m_geometryDirty.
        update();

        event->accept();
    }
}

void UnifiedGridRenderer::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_isDragging) {
        m_isDragging = false;

        // 🚀 DATA PAN LOGIC (happens once at the end)
        if (width() > 0 && height() > 0 && m_panVisualOffset.manhattanLength() > 0) {
            std::lock_guard<std::mutex> lock(m_dataMutex);

            // Convert the final visual offset (in pixels) to data coordinates (time and price)
            int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            double priceRange = m_maxPrice - m_minPrice;
            double timePixelsToMs = static_cast<double>(timeRange) / width();
            double pricePixelsToUnits = priceRange / height();

            int64_t timeDelta = static_cast<int64_t>(-m_panVisualOffset.x() * timePixelsToMs);
            double priceDelta = m_panVisualOffset.y() * pricePixelsToUnits;

            // Apply the delta to the main viewport
            m_visibleTimeStart_ms += timeDelta;
            m_visibleTimeEnd_ms += timeDelta;
            m_minPrice += priceDelta;
            m_maxPrice += priceDelta;

            m_panOffsetTime_ms += timeDelta;
            m_panOffsetPrice += priceDelta;
        }

        int64_t totalDragTime = m_interactionTimer.elapsed();
        
        // Reset the visual offset now that the data viewport is updated
        m_panVisualOffset = {0.0, 0.0};

        // Trigger a full data refresh for the new viewport area
        m_geometryDirty.store(true);
        update();

        // --- Existing Click-to-center logic (can remain) ---
        QPointF releasePos = event->position();
        QPointF dragDistance = releasePos - m_initialMousePos;
        double dragPixels = std::sqrt(dragDistance.x() * dragDistance.x() + dragDistance.y() * dragDistance.y());

        if (totalDragTime < 200 && dragPixels < 10.0 && m_timeWindowValid) {
            // Short click - center the viewport on this point
            double clickXRatio = releasePos.x() / width();
            double clickYRatio = 1.0 - (releasePos.y() / height()); // Invert Y for price axis
            int64_t clickTime;
            double clickPrice;

            {
                std::lock_guard<std::mutex> lock(m_dataMutex);
                int64_t timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
                double priceRange = m_maxPrice - m_minPrice;
                clickTime = m_visibleTimeStart_ms + static_cast<int64_t>(clickXRatio * timeRange);
                clickPrice = m_minPrice + (clickYRatio * priceRange);
                m_visibleTimeStart_ms = clickTime - timeRange / 2;
                m_visibleTimeEnd_ms = clickTime + timeRange / 2;
                m_minPrice = clickPrice - priceRange / 2.0;
                m_maxPrice = clickPrice + priceRange / 2.0;
            }

            m_geometryDirty.store(true);
            update();
            sLog_Chart("🎯 Click to center - centered on: " << clickTime << "ms, $" << clickPrice);
        } else {
            sLog_Chart("🖱️ Mouse drag completed - total time: " << totalDragTime << "ms");
        }

        event->accept();
    }
}

void UnifiedGridRenderer::wheelEvent(QWheelEvent* event) {
    // 🐛 FIX: Only handle wheel events when visible and in grid mode
    if (!isVisible() || !m_timeWindowValid || width() <= 0 || height() <= 0) {
        event->ignore();  // Let other components handle it
        return;
    }
    
    // 🐛 FIX: Check if wheel event is over UI control areas
    QPointF pos = event->position();
    if (pos.x() > width() * 0.7 && pos.y() < height() * 0.4) {
        event->ignore();  // Let UI controls handle it  
        return;
    }
    
    // 🚀 USER CONTROL: Disable auto-scroll when user manually zooms  
    m_autoScrollEnabled = false;
    
    // Zoom sensitivity settings
    const double ZOOM_SENSITIVITY = 0.001;
    const int64_t MIN_TIME_RANGE = 1000;     // 1 second minimum
    const int64_t MAX_TIME_RANGE = 600000;   // 10 minutes maximum  
    const double MIN_PRICE_RANGE = 1.0;      // $1 minimum
    const double MAX_PRICE_RANGE = 10000.0;  // $10k maximum
    
    // Calculate zoom delta
    double delta = event->angleDelta().y() * ZOOM_SENSITIVITY;
    double zoomFactor = 1.0 - delta; // Invert for natural zoom direction
    
    // Get cursor position for zoom center
    QPointF cursorPos = event->position();
    double cursorXRatio = cursorPos.x() / width();
    double cursorYRatio = 1.0 - (cursorPos.y() / height()); // Invert Y for price axis
    
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        
        // ──── TIME AXIS ZOOM (X) ────
        int64_t currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        int64_t newTimeRange = static_cast<int64_t>(currentTimeRange * zoomFactor);
        newTimeRange = std::max(MIN_TIME_RANGE, std::min(MAX_TIME_RANGE, newTimeRange));
        
        if (std::abs(newTimeRange - currentTimeRange) > 10) {
            // Calculate cursor position in time coordinates
            int64_t cursorTimestamp = m_visibleTimeStart_ms + static_cast<int64_t>(cursorXRatio * currentTimeRange);
            
            // Calculate new time window bounds around cursor
            int64_t timeDelta = newTimeRange - currentTimeRange;
            int64_t startAdjust = static_cast<int64_t>(timeDelta * cursorXRatio);
            int64_t endAdjust = static_cast<int64_t>(timeDelta * (1.0 - cursorXRatio));
            
            m_visibleTimeStart_ms -= startAdjust;
            m_visibleTimeEnd_ms += endAdjust;
        }
        
        // ──── PRICE AXIS ZOOM (Y) ────  
        double currentPriceRange = m_maxPrice - m_minPrice;
        double newPriceRange = currentPriceRange * zoomFactor;
        newPriceRange = std::max(MIN_PRICE_RANGE, std::min(MAX_PRICE_RANGE, newPriceRange));
        
        if (std::abs(newPriceRange - currentPriceRange) > 0.01) {
            // Calculate cursor position in price coordinates
            double cursorPrice = m_minPrice + (cursorYRatio * currentPriceRange);
            
            // Calculate new price window bounds around cursor
            double priceDelta = newPriceRange - currentPriceRange;
            double minAdjust = priceDelta * cursorYRatio;
            double maxAdjust = priceDelta * (1.0 - cursorYRatio);
            
            m_minPrice -= minAdjust;
            m_maxPrice += maxAdjust;
        }
        
        // Update internal zoom factor for consistency with keyboard controls
        m_zoomFactor = 60000.0 / newTimeRange;  // Normalize to base 60s range
        
        // 🚀 SMART CACHE REFRESH: Only refresh if zoom moved us outside cache bounds
        if (shouldRefreshCache()) {
            m_needsDataRefresh = true;
            sLog_Performance("🔍 ZOOM CACHE REFRESH: Viewport moved outside cache bounds");
        }
    }
    
    m_geometryDirty.store(true);
    update();
    emit autoScrollEnabledChanged(); // Notify UI that auto-scroll is disabled
    
    sLog_Chart("🖱️ Mouse wheel zoom - factor: " << m_zoomFactor 
             << " time range: " << (m_visibleTimeEnd_ms - m_visibleTimeStart_ms) << "ms"
             << " price range: $" << (m_maxPrice - m_minPrice));
    
    event->accept();
}

// 📊 PERFORMANCE MONITORING API

void UnifiedGridRenderer::togglePerformanceOverlay() {
    m_perfMetrics.showOverlay = !m_perfMetrics.showOverlay;
    sLog_Performance("📊 Performance overlay: " << (m_perfMetrics.showOverlay ? "ENABLED" : "DISABLED"));
    update();
}

QString UnifiedGridRenderer::getPerformanceStats() const {
    return QString(
        "=== SENTINEL PERFORMANCE STATS ===\n"
        "FPS: %.1f\n"
        "Avg Render Time: %.2f ms\n"
        "Cache Hit Rate: %.1f%%\n"
        "Cache Hits: %1\n"
        "Cache Misses: %2\n"
        "Geometry Rebuilds: %3\n"
        "Transform Operations: %4\n"
        "Cached Cells: %5\n"
        "Last Frame Time: %.2f ms\n"
        "GPU Bandwidth Est: %.1f MB/s\n"
    ).arg(m_perfMetrics.cacheHits)
     .arg(m_perfMetrics.cacheMisses)
     .arg(m_perfMetrics.geometryRebuilds)
     .arg(m_perfMetrics.transformsApplied)
     .arg(m_perfMetrics.cachedCellCount)
     .arg(m_perfMetrics.getCurrentFPS(), 0, 'f', 1)
     .arg(m_perfMetrics.getAverageRenderTime_ms(), 0, 'f', 2)
     .arg(m_perfMetrics.getCacheHitRate(), 0, 'f', 1)
     .arg(m_perfMetrics.renderTime_us / 1000.0, 0, 'f', 2)
     .arg((m_bytesUploadedThisFrame / 1024.0 / 1024.0) * m_perfMetrics.getCurrentFPS(), 0, 'f', 1);
}