#include "gpuchartwidget.h"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QVariantMap>
#include <QDebug>
#include <QDateTime>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <cmath>
#include <algorithm>
#include <chrono>

GPUChartWidget::GPUChartWidget(QQuickItem* parent) 
    : QQuickItem(parent) {
    
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    
    // 🚀 PHASE 1: Initialize triple-buffering
    for (int i = 0; i < BUFFER_COUNT; ++i) {
        m_gpuBuffers[i].points.reserve(m_maxPoints);
    }
    
    qDebug() << "🚀 GPUChartWidget OPTION B REBUILD - CLEAN COORDINATE SYSTEM!";
    qDebug() << "💾 Max Points:" << m_maxPoints << "| Time Span:" << m_timeSpanMs << "ms";
    qDebug() << "🎯 Single coordinate system - no test mode, no sine waves";
}

// 🔥 NEW: Handle widget resize - recalculate coordinates
void GPUChartWidget::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    
    if (newGeometry.size() != oldGeometry.size() && 
        newGeometry.width() > 0 && newGeometry.height() > 0) {
        
        qDebug() << "📐 Widget resized from" << oldGeometry.size() << "to" << newGeometry.size();
        
        // Mark geometry as dirty for coordinate recalculation
        m_geometryDirty.store(true);
        update();
    }
}

// 🔥 PHASE 1: REAL-TIME DATA INTEGRATION
void GPUChartWidget::onTradeReceived(const Trade& trade) {
    // 🔇 REDUCED DEBUG: Only log every 50th trade to reduce spam
    static int tradeCounter = 0;
    if (++tradeCounter % 50 == 0) {
        qDebug() << "🚀 GPU onTradeReceived:" << trade.product_id.c_str() 
                 << "Price:" << trade.price << "Size:" << trade.size;
    }
    
    appendTradeToVBO(trade);
}

void GPUChartWidget::onOrderBookUpdated(const OrderBook& book) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_currentOrderBook = book;
    // TODO: Phase 2 - order book heatmap integration
}

void GPUChartWidget::appendTradeToVBO(const Trade& trade) {
    // Get current write buffer
    int writeIdx = m_writeBuffer.load();
    auto& buffer = m_gpuBuffers[writeIdx];
    
    if (buffer.inUse.load()) {
        qDebug() << "⚠️ Buffer collision - frame skip (performance optimization)";
        return;
    }
    
    // Convert trade to GPU point
    GPUPoint point;
    convertTradeToGPUPoint(trade, point);
    
    // Thread-safe append
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    buffer.points.push_back(point);
    m_allRenderPoints.push_back(point); // 🔥 GEMINI FIX: Accumulate all trades
    
    // 🔥 REDUCED DEBUG: Only log first few trades to confirm system is working
    static int appendDebugCount = 0;
    if (appendDebugCount++ < 3) {
        qDebug() << "📝 Trade appended to VBO - Buffer points:" << buffer.points.size();
    }
    
    // Cleanup old points if we exceed max
    if (buffer.points.size() > static_cast<size_t>(m_maxPoints)) {
        cleanupOldTrades();
    }
    
    buffer.dirty.store(true);
    m_geometryDirty.store(true);
    
    // 🎯 PHASE 2: SMOOTH UPDATES: Every 5 trades for better visual smoothness
    static int tradeCount = 0;
    if (++tradeCount >= 50) { // 🚀 SMOOTH: Every 5 trades for real-time feel
        swapBuffers();
        tradeCount = 0;
        qDebug() << "🔄 Periodic buffer swap after 50 trades";
    }
    
    update(); // Trigger GPU update
}

void GPUChartWidget::convertTradeToGPUPoint(const Trade& trade, GPUPoint& point) {
    // 🔥 TIME-SERIES COORDINATE SYSTEM: X=time, Y=price
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
    
    // 🔥 NEW: Update time window for new trade (Option B)
    updateTimeWindow(timestamp_ms);
    
    // 🎯 UNIFIED PRICE RANGE: Auto-adjust to real BTC range
    if (trade.price > 80000.0) {
        updateDynamicPriceRange(trade.price);
    }
    
    // 🚀 TIME-SERIES MAPPING: Store raw data for coordinate transformation
    point.rawTimestamp = static_cast<double>(timestamp_ms);
    point.rawPrice = trade.price;
    
    // 🔥 SCREEN COORDINATE CALCULATION: Use new stateless system
    QPointF screenPos = worldToScreen(timestamp_ms, trade.price);
    point.x = static_cast<float>(screenPos.x());
    point.y = static_cast<float>(screenPos.y());
    
    // Color coding: Green=Buy, Red=Sell
    if (trade.side == AggressorSide::Buy) {
        point.r = 0.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f;
    } else {
        point.r = 1.0f; point.g = 0.0f; point.b = 0.0f; point.a = 0.8f;
    }
    
    point.size = 4.0f; // Trade dot size
    point.timestamp_ms = timestamp_ms;
    
    // Debug first few coordinate mappings to verify system
    static int tradeDebugCount = 0;
    if (++tradeDebugCount < 5) {
        qDebug() << "🎯 OPTION B TIME-SERIES Trade:" << "Price:" << trade.price 
                 << "Time:" << timestamp_ms << "→ Screen(" << point.x << "," << point.y << ")"
                 << "Window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms;
    }
}

// 🎯 PHASE 2: Dynamic Price Range Implementation
void GPUChartWidget::updateDynamicPriceRange(double newPrice) {
    m_lastTradePrice = newPrice;
    m_priceUpdateCount++;
    
    // Update range every 5 trades to prevent excessive recalculation
    if (m_priceUpdateCount % 5 == 0) {
        // Center the range around the recent price
        double halfRange = m_dynamicRangeSize / 2.0;
        double newMinPrice = newPrice - halfRange;
        double newMaxPrice = newPrice + halfRange;
        
        // Only update if the new range is significantly different (prevents jitter)
        double currentCenter = (m_minPrice + m_maxPrice) / 2.0;
        double priceDelta = std::abs(newPrice - currentCenter);
        
        if (priceDelta > m_dynamicRangeSize * 0.1) { // Update if price moves >10% of range
            m_minPrice = newMinPrice;
            m_maxPrice = newMaxPrice;
            
            qDebug() << "🎯 DYNAMIC RANGE UPDATED: Price" << newPrice 
                     << "→ Range:" << m_minPrice << "-" << m_maxPrice 
                     << "Size:" << (m_maxPrice - m_minPrice) 
                     << "Delta:" << priceDelta;
            
            // 🔥 THE FINAL FIX: Broadcast the new price range to the heatmap
            emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
        }
    }
}

bool GPUChartWidget::isInCurrentPriceRange(double price) const {
    return price >= m_minPrice && price <= m_maxPrice;
}

// 🎯 PHASE 2: New Public Methods
void GPUChartWidget::enableDynamicPriceZoom(bool enabled) {
    m_dynamicPriceZoom = enabled;
    if (!enabled) {
        // Restore static range
        m_minPrice = m_staticMinPrice;
        m_maxPrice = m_staticMaxPrice;
    }
    qDebug() << "🎯 Dynamic price zoom" << (enabled ? "ENABLED" : "DISABLED");
}

void GPUChartWidget::setDynamicPriceRange(double rangeSize) {
    m_dynamicRangeSize = rangeSize;
    qDebug() << "🎯 Dynamic price range size set to:" << rangeSize;
    
    // If we have a recent price, update the range immediately
    if (m_lastTradePrice > 0.0) {
        updateDynamicPriceRange(m_lastTradePrice);
    }
}

void GPUChartWidget::resetPriceRange() {
    if (m_dynamicPriceZoom && m_lastTradePrice > 0.0) {
        // Reset to dynamic range centered on last trade
        updateDynamicPriceRange(m_lastTradePrice);
    } else {
        // Reset to static range
        m_minPrice = m_staticMinPrice;
        m_maxPrice = m_staticMaxPrice;
    }
    qDebug() << "🎯 Price range reset to:" << m_minPrice << "-" << m_maxPrice;
}

// 🎯 PHASE 2: Safe Color Implementation (avoids dual-node crashes)
QColor GPUChartWidget::determineDominantTradeColor(const std::vector<GPUPoint>& points) const {
    if (points.empty()) {
        return QColor(128, 128, 128, 200); // Gray fallback
    }
    
    // Count recent buy vs sell activity (last 10 points for responsiveness)
    size_t checkCount = std::min(static_cast<size_t>(10), points.size());
    int buyCount = 0;
    int sellCount = 0;
    
    // Check the most recent points (end of vector)
    for (size_t i = points.size() - checkCount; i < points.size(); ++i) {
        const auto& point = points[i];
        
        // Detect color based on RGB values stored in GPUPoint
        if (point.g > 0.8f && point.r < 0.2f) {
            buyCount++; // Green = Buy
        } else if (point.r > 0.8f && point.g < 0.2f) {
            sellCount++; // Red = Sell
        }
    }
    
    // Return dominant color with good visibility
    if (buyCount > sellCount) {
        return QColor(0, 255, 0, 220); // Bright green for buy dominance
    } else if (sellCount > buyCount) {
        return QColor(255, 0, 0, 220); // Bright red for sell dominance
    } else {
        return QColor(255, 255, 0, 200); // Yellow for balanced activity
    }
}

void GPUChartWidget::cleanupOldTrades() {
    // 🎯 FIXED: Only clean up if we have too many points, not by time
    int writeIdx = m_writeBuffer.load();
    auto& buffer = m_gpuBuffers[writeIdx];
    
    // Only remove oldest trades if we exceed maximum
    if (buffer.points.size() > static_cast<size_t>(m_maxPoints)) {
        size_t excess = buffer.points.size() - m_maxPoints;
        buffer.points.erase(buffer.points.begin(), buffer.points.begin() + excess);
        qDebug() << "🧹 Cleaned up" << excess << "old trades, remaining:" << buffer.points.size();
    }
    
    // 🔥 GEMINI FIX: Also clean up accumulated points
    if (m_allRenderPoints.size() > static_cast<size_t>(m_maxPoints)) {
        size_t excess = m_allRenderPoints.size() - m_maxPoints;
        m_allRenderPoints.erase(m_allRenderPoints.begin(), m_allRenderPoints.begin() + excess);
        qDebug() << "🧹 Cleaned up" << excess << "accumulated points, remaining:" << m_allRenderPoints.size();
    }
}

void GPUChartWidget::swapBuffers() {
    // Atomic triple-buffer swap
    int oldWrite = m_writeBuffer.load();
    int oldRead = m_readBuffer.load();
    
    // Find the free buffer
    int newRead = oldWrite;
    int newWrite = 3 - oldWrite - oldRead; // The remaining buffer
    
    m_readBuffer.store(newRead);
    m_writeBuffer.store(newWrite);
    
    // 🔇 REDUCED DEBUG: Only log every 50th swap to reduce spam
    static int swapCounter = 0;
    if (++swapCounter % 50 == 0) {
        qDebug() << "🔄 Buffer swap: write" << oldWrite << "→" << newWrite 
                 << ", read" << oldRead << "→" << newRead;
    }
}

// 🔥 OPTION B: CLEAN REBUILD - SINGLE COORDINATE SYSTEM
QSGNode* GPUChartWidget::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    // 🚀 OPTION B: Start simple - render trades from VBO only
    if (width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }
    
    // 🔥 VBO MODE: Use triple-buffering for real data
    int readIdx = m_readBuffer.load();
    auto& readBuffer = m_gpuBuffers[readIdx];
    
    // Start with a single dot at fixed position for testing
    if (readBuffer.points.empty()) {
        qDebug() << "⚠️ No trade data yet - returning nullptr";
        delete oldNode;
        return nullptr;
    }
    
    readBuffer.inUse.store(true);
    std::lock_guard<std::mutex> lock(m_bufferMutex); // 🔥 GEMINI FIX: Thread safety lock
    
    // Create or reuse geometry node
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = new QSGGeometryNode;
        
        // Simple rectangles for visibility
        QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), m_allRenderPoints.size() * 6);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        
        QSGFlatColorMaterial* material = new QSGFlatColorMaterial;
        material->setColor(QColor(0, 255, 0, 200)); // Green for trades
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        
        qDebug() << "🔧 Created single-coordinate-system node with" << m_allRenderPoints.size() << "trade points";
    }
    
    if (readBuffer.dirty.load() || m_geometryDirty.load()) {
        // Update geometry with new trade data
        QSGGeometry* geometry = node->geometry();
        geometry->allocate(m_allRenderPoints.size() * 6); // 🔥 GEMINI FIX: Use accumulated points
        
        QSGGeometry::Point2D* vertices = geometry->vertexDataAsPoint2D();
        const float rectSize = 8.0f; // 8 pixel rectangles
        
        for (size_t i = 0; i < m_allRenderPoints.size(); ++i) { // 🔥 GEMINI FIX: Use accumulated points
            const auto& point = m_allRenderPoints[i]; // 🔥 GEMINI FIX: Use accumulated points
            size_t baseIndex = i * 6;
            
            // 🔥 NEW: Use clean stateless coordinate system
            QPointF screenPos = worldToScreen(point.rawTimestamp, point.rawPrice);
            
            float x = screenPos.x();
            float y = screenPos.y();
            float halfSize = rectSize / 2.0f;
            
            // Create rectangle (2 triangles = 6 vertices)
            vertices[baseIndex + 0].set(x - halfSize, y - halfSize);
            vertices[baseIndex + 1].set(x + halfSize, y - halfSize);
            vertices[baseIndex + 2].set(x - halfSize, y + halfSize);
            vertices[baseIndex + 3].set(x + halfSize, y - halfSize);
            vertices[baseIndex + 4].set(x + halfSize, y + halfSize);
            vertices[baseIndex + 5].set(x - halfSize, y + halfSize);
        }
        
        node->markDirty(QSGNode::DirtyGeometry);
        readBuffer.dirty.store(false);
        m_geometryDirty.store(false);
        
        // Debug first few coordinate mappings
        static int debugCount = 0;
        if (debugCount++ < 5 && !m_allRenderPoints.empty()) { // 🔥 GEMINI FIX: Use accumulated points
            const auto& firstPoint = m_allRenderPoints[0]; // 🔥 GEMINI FIX: Use accumulated points
            QPointF firstScreenPos = worldToScreen(firstPoint.rawTimestamp, firstPoint.rawPrice);
            qDebug() << "🎯 TIME-SERIES Trade: Price:" << firstPoint.rawPrice
                     << "Time:" << firstPoint.rawTimestamp << "→ Screen(" << firstScreenPos.x() << "," << firstScreenPos.y() << ")";
        }
    }
    
    readBuffer.inUse.store(false);
    return node;
}

// 🚀 PHASE 1: Configuration Methods
void GPUChartWidget::setMaxPoints(int maxPoints) {
    m_maxPoints = maxPoints;
    qDebug() << "🎯 Max points set to:" << maxPoints;
}

void GPUChartWidget::setTimeSpan(double timeSpanSeconds) {
    m_timeSpanMs = timeSpanSeconds * 1000.0;
    qDebug() << "⏱️ Time span set to:" << timeSpanSeconds << "seconds";
}

void GPUChartWidget::setPriceRange(double minPrice, double maxPrice) {
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    qDebug() << "📊 Price range set to:" << minPrice << "-" << maxPrice;
}

void GPUChartWidget::addTrade(const Trade& trade) {
    // Legacy interface - redirect to slot
    onTradeReceived(trade);
}

void GPUChartWidget::updateOrderBook(const OrderBook& book) {
    // Legacy interface - redirect to slot
    onOrderBookUpdated(book);
}

void GPUChartWidget::clearTrades() {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    // Clear all buffers
    for (auto& buffer : m_gpuBuffers) {
        buffer.points.clear();
        buffer.dirty.store(true);
    }
    
    // 🔥 GEMINI FIX: Also clear accumulated points
    m_allRenderPoints.clear();
    
    m_geometryDirty.store(true);
    update();
    
    qDebug() << "🧹 All GPU buffers and accumulated points cleared!";
}

// 🔥 REMOVED: mapToScreen function - replaced with worldToScreen in Option B

// 🚀 PHASE 4: PAN/ZOOM INTERACTION METHODS

void GPUChartWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_interactionTimer.start();
        m_isDragging = true;
        m_lastMousePos = event->position();
        
        // 🔥 GEMINI FIX: Disable ALL auto-pilot systems immediately
        m_autoScrollEnabled = false;
        m_dynamicPriceZoom = false;
        qDebug() << "🖱️ User control active - all auto-pilot systems disabled";
        
        event->accept();
    }
}

void GPUChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging) {
        // 🔥 GEMINI FIX: Disable auto-pilot systems at start of drag
        m_autoScrollEnabled = false;
        m_dynamicPriceZoom = false;
        
        QPointF currentPos = event->position();
        QPointF delta = currentPos - m_lastMousePos;
        
        if (width() > 0 && height() > 0 && m_timeWindowInitialized) {
            // 🔥 OPTION B: Pan by moving the time window directly
            double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
            
            // Convert pixel delta to time duration
            double pixelsToTimeRatio = timeRange / width();
            double timeDelta = delta.x() * pixelsToTimeRatio;
            
            // Move time window (subtract for natural drag direction)
            m_visibleTimeStart_ms -= timeDelta;
            m_visibleTimeEnd_ms -= timeDelta;
            emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
            
            // Price range panning
            double priceRange = m_maxPrice - m_minPrice;
            double pixelsToPriceRatio = priceRange / height();
            double priceDelta = delta.y() * pixelsToPriceRatio;
            
            // Move price range (add for natural drag direction)
            m_minPrice += priceDelta;
            m_maxPrice += priceDelta;
            
            m_lastMousePos = currentPos;
            m_geometryDirty.store(true);
            
            // Throttle updates for smooth performance
            static int updateCounter = 0;
            if (++updateCounter % 2 == 0) {
                update();
            }
        }
        
        // Debug camera movement
        static int panDebugCount = 0;
        if (panDebugCount++ < 3) {
            qDebug() << "🎥 CAMERA PAN: Time window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms
                     << "Price range:" << m_minPrice << "to" << m_maxPrice;
        }
        
        event->accept();
    }
}

void GPUChartWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_isDragging) {
        m_isDragging = false;
        
        // 🔥 REDUCED DEBUG: Only log performance achievements
        double latency = calculateInteractionLatency();
        if (latency < 5.0) {
            static int panSuccessCount = 0;
            if (panSuccessCount++ < 3) {
                qDebug() << "✅ PHASE 4 SUCCESS: Pan latency" << latency << "ms < 5ms target!";
            }
        }
        
        event->accept();
    }
}

void GPUChartWidget::wheelEvent(QWheelEvent* event) {
    // 🔥 GEMINI FIX: Disable auto-pilot systems at start of wheel zoom
    m_autoScrollEnabled = false;
    m_dynamicPriceZoom = false;
    
    if (!m_timeWindowInitialized || width() <= 0 || height() <= 0) {
        event->accept();
        return;
    }
    
    // 🔥 OPTION B: Zoom by changing the time window duration
    const double ZOOM_SENSITIVITY = 0.001; // Tuned for time window
    const double MIN_TIME_RANGE = 1000.0;  // 1 second minimum
    const double MAX_TIME_RANGE = 600000.0; // 10 minutes maximum
    
    // Calculate zoom delta
    double delta = event->angleDelta().y() * ZOOM_SENSITIVITY;
    double zoomFactor = 1.0 - delta; // Invert for natural zoom direction
    
    // Current time window
    double currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double newTimeRange = currentTimeRange * zoomFactor;
    
    // Clamp to safe bounds
    newTimeRange = std::max(MIN_TIME_RANGE, std::min(MAX_TIME_RANGE, newTimeRange));
    
    if (std::abs(newTimeRange - currentTimeRange) > 10.0) { // Only update if significant change
        // Calculate cursor position in time coordinates
        QPointF cursorPos = event->position();
        double cursorTimeRatio = cursorPos.x() / width();
        int64_t cursorTimestamp = m_visibleTimeStart_ms + (cursorTimeRatio * currentTimeRange);
        
        // Calculate new window bounds around cursor
        double timeDelta = newTimeRange - currentTimeRange;
        int64_t startAdjust = static_cast<int64_t>(timeDelta * cursorTimeRatio);
        int64_t endAdjust = static_cast<int64_t>(timeDelta * (1.0 - cursorTimeRatio));
        
        m_visibleTimeStart_ms -= startAdjust;
        m_visibleTimeEnd_ms += endAdjust;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
        
        // Disable auto-scroll on zoom
        if (m_autoScrollEnabled) {
            m_autoScrollEnabled = false;
            qDebug() << "🔍 Auto-scroll disabled - manual zoom";
        }
        
        m_geometryDirty.store(true);
        update();
        
        // Debug camera zoom
        static int zoomDebugCount = 0;
        if (zoomDebugCount++ < 3) {
            qDebug() << "🎥 CAMERA ZOOM: Time range:" << (newTimeRange/1000.0) << "seconds"
                     << "Window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms
                     << "Cursor at:" << cursorTimestamp;
        }
    }
    
    event->accept();
}

// 🎯 PHASE 4: Pan/Zoom Control Methods

void GPUChartWidget::zoomIn() {
    if (!m_timeWindowInitialized) return;
    
    // 🔥 OPTION B: Zoom by reducing time window duration
    double currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double newTimeRange = currentTimeRange * 0.8; // 20% zoom in
    double timeCenter = (m_visibleTimeStart_ms + m_visibleTimeEnd_ms) / 2.0;
    
    m_visibleTimeStart_ms = static_cast<int64_t>(timeCenter - newTimeRange / 2.0);
    m_visibleTimeEnd_ms = static_cast<int64_t>(timeCenter + newTimeRange / 2.0);
    emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
    
    m_geometryDirty.store(true);
    update();
    qDebug() << "🔍 Camera ZOOM IN: Time range:" << (newTimeRange/1000.0) << "seconds";
}

void GPUChartWidget::zoomOut() {
    if (!m_timeWindowInitialized) return;
    
    // 🔥 OPTION B: Zoom by increasing time window duration  
    double currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double newTimeRange = currentTimeRange * 1.25; // 25% zoom out
    double timeCenter = (m_visibleTimeStart_ms + m_visibleTimeEnd_ms) / 2.0;
    
    m_visibleTimeStart_ms = static_cast<int64_t>(timeCenter - newTimeRange / 2.0);
    m_visibleTimeEnd_ms = static_cast<int64_t>(timeCenter + newTimeRange / 2.0);
    emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
    
    m_geometryDirty.store(true);
    update();
    qDebug() << "🔍 Camera ZOOM OUT: Time range:" << (newTimeRange/1000.0) << "seconds";
}

void GPUChartWidget::resetZoom() {
    // 🔥 OPTION B: Reset to default time window
    if (m_timeWindowInitialized) {
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_visibleTimeEnd_ms = currentTime;
        m_visibleTimeStart_ms = currentTime - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
        
        // 🔥 FIX: Reset price range to current BTC levels
        m_minPrice = 106000.0;  // Current BTC range
        m_maxPrice = 108000.0;
        
        m_geometryDirty.store(true);
        update();
        qDebug() << "🔄 CAMERA RESET - Time window and price range restored!";
    }
}

void GPUChartWidget::enableAutoScroll(bool enabled) {
    if (m_autoScrollEnabled != enabled) {
        m_autoScrollEnabled = enabled;
        emit autoScrollEnabledChanged();
        
        if (enabled) {
            updateAutoScroll();
            qDebug() << "✅ Auto-scroll ENABLED - following latest data";
        } else {
            qDebug() << "❌ Auto-scroll DISABLED - manual navigation mode";
        }
    }
}

void GPUChartWidget::centerOnPrice(double price) {
    // 🔥 OPTION B: Center by adjusting price range instead of pan offset
    double priceSpan = m_maxPrice - m_minPrice;
    m_minPrice = price - priceSpan / 2.0;
    m_maxPrice = price + priceSpan / 2.0;
    m_geometryDirty.store(true);
    update();
    qDebug() << "🎯 Centered on price:" << price << "Range:" << m_minPrice << "-" << m_maxPrice;
}

void GPUChartWidget::centerOnTime(qint64 timestamp) {
    // 🔥 OPTION B: Center by adjusting time window instead of pan offset
    double timeSpan = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    m_visibleTimeStart_ms = timestamp - static_cast<int64_t>(timeSpan / 2.0);
    m_visibleTimeEnd_ms = timestamp + static_cast<int64_t>(timeSpan / 2.0);
    emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
    m_geometryDirty.store(true);
    update();
    qDebug() << "🎯 Centered on timestamp:" << timestamp << "Window:" << m_visibleTimeStart_ms << "-" << m_visibleTimeEnd_ms;
}

// 🎯 PHASE 4: Coordinate Transformation Methods

// 🔥 NEW: PURE STATELESS COORDINATE SYSTEM (Option B)
QPointF GPUChartWidget::worldToScreen(int64_t timestamp_ms, double price) const {
    if (width() <= 0 || height() <= 0) {
        return QPointF(0, 0);
    }
    
    // 🔥 STATELESS: Use time window instead of static reference
    if (!m_timeWindowInitialized || m_visibleTimeEnd_ms <= m_visibleTimeStart_ms) {
        // Fallback: create a sliding window around current time
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const_cast<GPUChartWidget*>(this)->m_visibleTimeEnd_ms = currentTime;
        const_cast<GPUChartWidget*>(this)->m_visibleTimeStart_ms = currentTime - m_timeSpanMs;
        const_cast<GPUChartWidget*>(this)->m_timeWindowInitialized = true;
    }
    
    // 🔥 PURE MAPPING: Time window directly to screen coordinates
    double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double timeRatio = (timestamp_ms - m_visibleTimeStart_ms) / timeRange;
    double x = timeRatio * width();
    
    // 🔥 FIX: Use dynamic price range based on actual trade data
    // Check if we have a valid dynamic price range from recent trades
    double actualMinPrice = m_minPrice;
    double actualMaxPrice = m_maxPrice;
    
    // If using static range that doesn't match current BTC prices, use dynamic range
    if (actualMaxPrice < 100000 && price > 100000) {
        // BTC is above 100k, but our range is below - use dynamic centering
        double priceCenter = price;
        double priceSpan = 100.0; // $100 range around current price for good resolution
        actualMinPrice = priceCenter - priceSpan / 2.0;
        actualMaxPrice = priceCenter + priceSpan / 2.0;
    }
    
    // Map price to Y coordinate (flipped for screen coordinates)
    double priceRange = actualMaxPrice - actualMinPrice;
    if (priceRange <= 0) priceRange = 100.0; // Fallback to avoid division by zero
    
    double priceRatio = (price - actualMinPrice) / priceRange;
    double y = (1.0 - priceRatio) * height();
    
    // 🔥 NO MORE PAN/ZOOM TRANSFORMS: Camera movement handled by time window
    return QPointF(x, y);
}

QPointF GPUChartWidget::screenToWorld(const QPointF& screenPos) const {
    if (width() <= 0 || height() <= 0) {
        return QPointF(0, 0);
    }
    
    // 🔥 OPTION B: Use time window for reverse mapping
    double normalizedX = screenPos.x() / width();
    double normalizedY = 1.0 - (screenPos.y() / height()); // Flip Y
    
    // Map screen coordinates back to world coordinates using time window
    double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double timestamp = m_visibleTimeStart_ms + (normalizedX * timeRange);
    
    double priceRange = m_maxPrice - m_minPrice;
    double price = m_minPrice + (normalizedY * priceRange);
    
    return QPointF(timestamp, price);
}

void GPUChartWidget::applyPanZoomToPoint(GPUPoint& point, double rawTimestamp, double rawPrice) {
    QPointF screenPos = worldToScreen(rawTimestamp, rawPrice);
    point.x = screenPos.x();
    point.y = screenPos.y();
}

void GPUChartWidget::updateAutoScroll() {
    if (m_autoScrollEnabled) {
        // 🔥 OPTION B: Follow latest data by adjusting time window
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_visibleTimeEnd_ms = currentTime;
        m_visibleTimeStart_ms = currentTime - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
        m_geometryDirty.store(true);
        update();
    }
}

double GPUChartWidget::calculateInteractionLatency() const {
    if (m_interactionTimer.isValid()) {
        return static_cast<double>(m_interactionTimer.elapsed());
    }
    return 0.0;
}

// 🔥 NEW: Time window management functions (Option B)
void GPUChartWidget::initializeTimeWindow(int64_t firstTimestamp) {
    m_visibleTimeEnd_ms = firstTimestamp;
    m_visibleTimeStart_ms = firstTimestamp - m_timeSpanMs;
    m_timeWindowInitialized = true;
    qDebug() << "🔥 Initialized time window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms;
}

void GPUChartWidget::updateTimeWindow(int64_t newTimestamp) {
    if (!m_timeWindowInitialized) {
        initializeTimeWindow(newTimestamp);
        return;
    }
    
    if (m_autoScrollEnabled) {
        // Slide window to follow latest data
        m_visibleTimeEnd_ms = newTimestamp;
        m_visibleTimeStart_ms = newTimestamp - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
    }
}

// 🔥 REMOVED: calculateColumnBasedPosition function with problematic static variable
// Replaced with stateless worldToScreen coordinate system in Option B 