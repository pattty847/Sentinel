#include "gpuchartwidget.h"
#include "SentinelLogging.hpp"
#include <QSGGeometry>
#include <QSGFlatColorMaterial>
#include <QSGVertexColorMaterial>
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
    
    sLog_Init("🚀 GPUChartWidget OPTION B REBUILD - CLEAN COORDINATE SYSTEM!");
    sLog_Init("💾 Max Points:" << m_maxPoints << "| Time Span:" << m_timeSpanMs << "ms");
    sLog_Init("🎯 Single coordinate system - no test mode, no sine waves");
}

// 🔥 NEW: Handle widget resize - recalculate coordinates
void GPUChartWidget::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    
    if (newGeometry.size() != oldGeometry.size() && 
        newGeometry.width() > 0 && newGeometry.height() > 0) {
        
        sLog_Chart("📐 Widget resized from" << oldGeometry.size() << "to" << newGeometry.size());
        
        // Mark geometry as dirty for coordinate recalculation
        m_geometryDirty.store(true);
        update();
    }
}

// 🔥 PHASE 1: REAL-TIME DATA INTEGRATION
void GPUChartWidget::onTradeReceived(const Trade& trade) {
    // 🔥 CONNECTION TEST: Verify this method is being called
    static int connectionTestCount = 0;
    if (++connectionTestCount <= 5) {
        sLog_Trades("🎯 onTradeReceived CALLED! #" << connectionTestCount 
                 << "Price:" << trade.price << "Side:" << (int)trade.side);
    }
    
    // 🔍 DEBUG: Log raw trade data to see what sides we're receiving
    static int rawTradeDebugCount = 0;
    if (++rawTradeDebugCount <= 10) {
        const char* rawSideStr = "UNKNOWN";
        if (trade.side == AggressorSide::Buy) rawSideStr = "BUY";
        else if (trade.side == AggressorSide::Sell) rawSideStr = "SELL";
        
        sLog_Trades("📈 RAW TRADE DATA #" << rawTradeDebugCount << ":"
                 << "Product:" << trade.product_id.c_str()
                 << "Side:" << rawSideStr
                 << "Price:" << trade.price
                 << "Size:" << trade.size);
    }
    
    // 📊 TRADE DISTRIBUTION TRACKING
    static int totalBuys = 0, totalSells = 0, totalUnknown = 0;
    if (trade.side == AggressorSide::Buy) totalBuys++;
    else if (trade.side == AggressorSide::Sell) totalSells++;
    else totalUnknown++;
    
    // Log distribution every 25 trades
    static int distributionCounter = 0;
    if (++distributionCounter % 25 == 0) {
        int totalTrades = totalBuys + totalSells + totalUnknown;
        double buyPercent = totalTrades > 0 ? (totalBuys * 100.0 / totalTrades) : 0.0;
        double sellPercent = totalTrades > 0 ? (totalSells * 100.0 / totalTrades) : 0.0;
        
        sLog_Trades("📊 TRADE DISTRIBUTION SUMMARY:"
                 << "Total:" << totalTrades
                 << "Buys:" << totalBuys << "(" << QString::number(buyPercent, 'f', 1) << "%)"
                 << "Sells:" << totalSells << "(" << QString::number(sellPercent, 'f', 1) << "%)"
                 << "Unknown:" << totalUnknown);
    }
    
    appendTradeToVBO(trade);
}

void GPUChartWidget::onOrderBookUpdated(const OrderBook& book) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_currentOrderBook = book;
    
    // 🔥 ORDER BOOK VIEWPORT UPDATES: Use fastest update cadence for smooth viewport movement
    if (!book.bids.empty() || !book.asks.empty()) {
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        updateTimeWindow(currentTime);
    }
}

void GPUChartWidget::appendTradeToVBO(const Trade& trade) {
    // Get current write buffer
    int writeIdx = m_writeBuffer.load();
    auto& buffer = m_gpuBuffers[writeIdx];
    
    if (buffer.inUse.load()) {
        sLog_Performance("⚠️ Buffer collision - frame skip (performance optimization)");
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
        // sLog_Gpu() << "📝 Trade appended to VBO - Buffer points:" << buffer.points.size();  // Too chatty!
    }
    
    // Cleanup old points if we exceed max
    if (buffer.points.size() > static_cast<size_t>(m_maxPoints)) {
        cleanupOldTrades();
    }
    
    buffer.dirty.store(true);
    m_geometryDirty.store(true);
    
    // 🎯 PHASE 2: SMOOTH UPDATES: Every 10 trades for better visual smoothness
    static int tradeCount = 0;
    if (++tradeCount >= 10) { // 🚀 SMOOTH: Every 10 trades for real-time feel
        swapBuffers();
        tradeCount = 0;
        // sLog_Gpu() << "🔄 Periodic buffer swap after 10 trades";  // Too chatty!
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
    
    // 🎨 THREE-COLOR PRICE CHANGE SYSTEM: Professional trading terminal style
    const char* changeStr = "FIRST";
    if (!m_hasPreviousPrice) {
        // First trade - use neutral yellow
        point.r = 1.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // YELLOW for FIRST
        changeStr = "FIRST";
        m_previousTradePrice = trade.price;
        m_hasPreviousPrice = true;
    } else {
        // Compare with previous price for uptick/downtick/no-change
        if (trade.price > m_previousTradePrice) {
            point.r = 0.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // GREEN for UPTICK
            changeStr = "UPTICK";
        } else if (trade.price < m_previousTradePrice) {
            point.r = 1.0f; point.g = 0.0f; point.b = 0.0f; point.a = 0.8f; // RED for DOWNTICK
            changeStr = "DOWNTICK";
        } else {
            point.r = 1.0f; point.g = 1.0f; point.b = 0.0f; point.a = 0.8f; // YELLOW for NO CHANGE
            changeStr = "NO_CHANGE";
        }
        m_previousTradePrice = trade.price;
    }

    point.size = 4.0f; // Trade dot size
    point.timestamp_ms = timestamp_ms;
    
    // 🔍 DEBUG: Log every trade's color assignment
    static int tradeColorDebugCount = 0;
    if (++tradeColorDebugCount <= 10) {
        sLog_Trades("🎨 TRADE COLOR DEBUG #" << tradeColorDebugCount << ":"
                 << "Change:" << changeStr
                 << "Price:" << trade.price << "(prev:" << m_previousTradePrice << ")"
                 << "Size:" << trade.size
                 << "Color RGBA:(" << point.r << "," << point.g << "," << point.b << "," << point.a << ")");
    }
    
    // Debug first few coordinate mappings to verify system
    static int tradeDebugCount = 0;
    if (++tradeDebugCount < 5) {
        // sLog_DebugCoords() << "🎯 OPTION B TIME-SERIES Trade:" << "Price:" << trade.price 
        //          << "Time:" << timestamp_ms << "→ Screen(" << point.x << "," << point.y << ")"
        //          << "Window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms;  // Too chatty!
    }
}

// 🎯 PHASE 2: Dynamic Price Range Implementation
void GPUChartWidget::updateDynamicPriceRange(double newPrice) {
    m_lastTradePrice = newPrice;
    m_priceUpdateCount++;
    
    // 🚀 USER CONTROL PRIORITY: Don't auto-center if user is manually controlling viewport
    // This prevents the annoying "snap back to center" behavior when price jumps
    if (!m_autoScrollEnabled || m_isDragging) {
        // User is in control - don't auto-adjust the price range
        return;
    }
    
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
            
            sLog_Chart("🎯 DYNAMIC RANGE UPDATED: Price" << newPrice 
                     << "→ Range:" << m_minPrice << "-" << m_maxPrice 
                     << "Size:" << (m_maxPrice - m_minPrice) 
                     << "Delta:" << priceDelta);
            
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
    sLog_Chart("🎯 Dynamic price zoom" << (enabled ? "ENABLED" : "DISABLED"));
}

void GPUChartWidget::setDynamicPriceRange(double rangeSize) {
    m_dynamicRangeSize = rangeSize;
    sLog_Chart("🎯 Dynamic price range size set to:" << rangeSize);
    
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
    sLog_Chart("🎯 Price range reset to:" << m_minPrice << "-" << m_maxPrice);
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
    }
    
    // 🔥 GEMINI FIX: Also clean up accumulated points
    if (m_allRenderPoints.size() > static_cast<size_t>(m_maxPoints)) {
        size_t excess = m_allRenderPoints.size() - m_maxPoints;
        m_allRenderPoints.erase(m_allRenderPoints.begin(), m_allRenderPoints.begin() + excess);
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
}

// 🔥 OPTION B: CLEAN REBUILD - SINGLE COORDINATE SYSTEM WITH PER-VERTEX COLORS
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
        sLog_Render("⚠️ No trade data yet - returning nullptr");
        delete oldNode;
        return nullptr;
    }
    
    readBuffer.inUse.store(true);
    std::lock_guard<std::mutex> lock(m_bufferMutex); // 🔥 GEMINI FIX: Thread safety lock
    
    // Create or reuse geometry node
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = new QSGGeometryNode;
        
        // 🔵 CIRCLE GEOMETRY: Use 8 triangles per circle (24 vertices per trade)
        QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), m_allRenderPoints.size() * 24);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        
        // 🔥 BREAKTHROUGH: QSGVertexColorMaterial reads per-vertex colors!
        QSGVertexColorMaterial* material = new QSGVertexColorMaterial;
        material->setFlag(QSGMaterial::Blending); // Enable alpha blending
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        
        sLog_RenderDetail("🔵 CREATED CIRCLE GEOMETRY: Three-color price change system with smooth circles!");
    }
    
    if (readBuffer.dirty.load() || m_geometryDirty.load()) {
        // 🚀 PHASE 4: GPU UPLOAD PROFILER - Reset frame counter
        m_bytesUploadedThisFrame = 0;
        
        // Update geometry with new trade data
        QSGGeometry* geometry = node->geometry();
        geometry->allocate(m_allRenderPoints.size() * 24); // 🔵 CIRCLES: 8 triangles × 3 vertices = 24 per circle
        
        // 🔵 CIRCLE GEOMETRY: Use ColoredPoint2D vertices for smooth circles!
        QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
        const float circleRadius = 6.0f; // 6 pixel radius circles (was 8px rectangles)
        const int trianglesPerCircle = 8; // Octagon approximation for performance
        
        for (size_t i = 0; i < m_allRenderPoints.size(); ++i) {
            const auto& point = m_allRenderPoints[i];
            size_t baseIndex = i * 24; // 24 vertices per circle
            
            // 🔥 NEW: Use clean stateless coordinate system
            QPointF screenPos = worldToScreen(point.rawTimestamp, point.rawPrice);
            
            float centerX = screenPos.x();
            float centerY = screenPos.y();
            
            // 🎨 CONVERT RGB TO UCHAR: ColoredPoint2D expects separate RGBA values
            uchar red   = static_cast<uchar>(point.r * 255);
            uchar green = static_cast<uchar>(point.g * 255);
            uchar blue  = static_cast<uchar>(point.b * 255);
            uchar alpha = static_cast<uchar>(point.a * 255);
            
            // 🔵 CREATE CIRCLE: Triangle fan from center to perimeter
            for (int t = 0; t < trianglesPerCircle; ++t) {
                size_t triangleBase = baseIndex + (t * 3);
                
                // Calculate angles for this triangle
                float angle1 = (t * 2.0f * M_PI) / trianglesPerCircle;
                float angle2 = ((t + 1) * 2.0f * M_PI) / trianglesPerCircle;
                
                // Center vertex
                vertices[triangleBase + 0].set(centerX, centerY, red, green, blue, alpha);
                
                // First perimeter vertex
                float x1 = centerX + circleRadius * cos(angle1);
                float y1 = centerY + circleRadius * sin(angle1);
                vertices[triangleBase + 1].set(x1, y1, red, green, blue, alpha);
                
                // Second perimeter vertex
                float x2 = centerX + circleRadius * cos(angle2);
                float y2 = centerY + circleRadius * sin(angle2);
                vertices[triangleBase + 2].set(x2, y2, red, green, blue, alpha);
            }
        }
        
        // 🚀 PHASE 4: GPU UPLOAD PROFILER - Track bytes sent to GPU
        size_t vertexCount = m_allRenderPoints.size() * 24;
        size_t bytesUploaded = vertexCount * sizeof(QSGGeometry::ColoredPoint2D);
        m_bytesUploadedThisFrame += bytesUploaded;
        m_totalBytesUploaded.fetch_add(bytesUploaded, std::memory_order_relaxed);
        
        // Calculate bandwidth and check budget
        m_mbPerFrame = static_cast<double>(m_bytesUploadedThisFrame) / 1'000'000.0;
        double estimatedFPS = 60.0; // Assume 60 FPS for trade scatter
        double bandwidthMBps = m_mbPerFrame * estimatedFPS;
        
        // Warn if exceeding PCIe budget
        if (bandwidthMBps > PCIE_BUDGET_MB_PER_SECOND) {
            m_bandwidthWarnings.fetch_add(1, std::memory_order_relaxed);
            static int warningCount = 0;
            if (++warningCount <= 5) { // Limit warning spam
                sLog_Performance("⚠️ TRADE SCATTER PCIe WARNING #" << warningCount
                          << "Current:" << QString::number(bandwidthMBps, 'f', 1) << "MB/s"
                          << "Budget:" << PCIE_BUDGET_MB_PER_SECOND << "MB/s"
                          << "Frame:" << QString::number(m_mbPerFrame, 'f', 3) << "MB"
                          << "Points:" << m_allRenderPoints.size());
            }
        }
        
        // 🚀 PHASE 4: Update debug display with profiler metrics
        updateDebugInfo();
        
        // Debug logging for first few frames
        static int profileCount = 0;
        if (++profileCount <= 10 || profileCount % 100 == 0) {
            sLog_Performance("📊 TRADE SCATTER GPU PROFILER #" << profileCount
                     << "Points:" << m_allRenderPoints.size()
                     << "Frame:" << QString::number(m_mbPerFrame, 'f', 3) << "MB"
                     << "Bandwidth:" << QString::number(bandwidthMBps, 'f', 1) << "MB/s"
                     << "Total uploaded:" << (m_totalBytesUploaded.load() / 1'000'000) << "MB");
        }
        
        node->markDirty(QSGNode::DirtyGeometry);
        readBuffer.dirty.store(false);
        m_geometryDirty.store(false);
        
        // 🔍 DEBUG: Check what colors are actually being rendered
        static int debugCount = 0;
        if (debugCount++ < 5 && !m_allRenderPoints.empty()) {
            // Count price change types in current buffer
            int uptickCount = 0, downtickCount = 0, noChangeCount = 0;
            for (const auto& point : m_allRenderPoints) {
                if (point.g > 0.8f && point.r < 0.2f) uptickCount++;        // Green = Uptick
                else if (point.r > 0.8f && point.g < 0.2f) downtickCount++;  // Red = Downtick  
                else noChangeCount++;                                         // Yellow = No Change
            }
            
            sLog_RenderDetail("🔵 CIRCLE RENDER DEBUG #" << debugCount << ":"
                     << "Total circles:" << m_allRenderPoints.size()
                     << "UPTICKS:" << uptickCount << "(green)"
                     << "DOWNTICKS:" << downtickCount << "(red)"  
                     << "NO_CHANGE:" << noChangeCount << "(yellow)");
                     
            // Show first few individual circle colors
            for (int i = 0; i < std::min(3, (int)m_allRenderPoints.size()); ++i) {
                const auto& point = m_allRenderPoints[i];
                const char* colorName = "NO_CHANGE";
                if (point.g > 0.8f && point.r < 0.2f) colorName = "GREEN-UPTICK";
                else if (point.r > 0.8f && point.g < 0.2f) colorName = "RED-DOWNTICK";
                else if (point.r > 0.8f && point.g > 0.8f) colorName = "YELLOW-NO_CHANGE";
                
                sLog_RenderDetail("  Circle" << i << ":" << colorName 
                         << "RGBA(" << point.r << "," << point.g << "," << point.b << "," << point.a << ")"
                         << "Price:" << point.rawPrice);
            }
        }
    }
    
    readBuffer.inUse.store(false);
    return node;
}

// 🚀 PHASE 1: Configuration Methods
void GPUChartWidget::setMaxPoints(int maxPoints) {
    m_maxPoints = maxPoints;
    sLog_Chart("🎯 Max points set to:" << maxPoints);
}

void GPUChartWidget::setTimeSpan(double timeSpanSeconds) {
    m_timeSpanMs = timeSpanSeconds * 1000.0;
    sLog_Chart("⏱️ Time span set to:" << timeSpanSeconds << "seconds");
}

void GPUChartWidget::setPriceRange(double minPrice, double maxPrice) {
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    sLog_Chart("📊 Price range set to:" << minPrice << "-" << maxPrice);
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
    
    sLog_Chart("🧹 All GPU buffers and accumulated points cleared!");
}

// 🔥 REMOVED: mapToScreen function - replaced with worldToScreen in Option B

// 🚀 PHASE 4: PAN/ZOOM INTERACTION METHODS

void GPUChartWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_interactionTimer.start();
        m_isDragging = true;
        m_lastMousePos = event->position();
        
        // 🚀 USER CONTROL: Disable auto-centering and auto-scroll immediately
        m_autoScrollEnabled = false;
        m_dynamicPriceZoom = false;
        sLog_Camera("🖱️ User control active - manual viewport navigation enabled");
        
        event->accept();
    }
}

void GPUChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging) {
        // 🚀 USER CONTROL: Ensure auto-systems stay disabled during drag
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
            
            // Price range panning
            double priceRange = m_maxPrice - m_minPrice;
            double pixelsToPriceRatio = priceRange / height();
            double priceDelta = delta.y() * pixelsToPriceRatio;
            
            // Move price range (add for natural drag direction)
            m_minPrice += priceDelta;
            m_maxPrice += priceDelta;
            
            m_lastMousePos = currentPos;
            m_geometryDirty.store(true);
            
            // 🔥 MEMORY LEAK FIX: Heavily throttle expensive operations during drag
            static int updateCounter = 0;
            static int viewChangeCounter = 0;
            
            // Update rendering every 4th mouse move for smooth visuals
            if (++updateCounter % 4 == 0) {
                update();
            }
            
            // Emit viewChanged only every 10th mouse move to prevent expensive recalculations
            if (++viewChangeCounter % 10 == 0) {
                emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
            }
        }
        
        // Debug camera movement
        static int panDebugCount = 0;
        if (panDebugCount++ < 3) {
            sLog_Camera("🎥 CAMERA PAN: Time window:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms
                     << "Price range:" << m_minPrice << "to" << m_maxPrice);
        }
        
        event->accept();
    }
}

void GPUChartWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_isDragging) {
        m_isDragging = false;
        
        // 🔥 MEMORY LEAK FIX: Emit final viewChanged to ensure components get final position
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
        
        // 🔥 REDUCED DEBUG: Only log performance achievements
        double latency = calculateInteractionLatency();
        if (latency < 5.0) {
            static int panSuccessCount = 0;
            if (panSuccessCount++ < 3) {
                sLog_Performance("✅ PHASE 4 SUCCESS: Pan latency" << latency << "ms < 5ms target!");
            }
        }
        
        event->accept();
    }
}

void GPUChartWidget::wheelEvent(QWheelEvent* event) {
    // 🚀 USER CONTROL: Disable auto-centering when user manually zooms
    m_autoScrollEnabled = false;
    m_dynamicPriceZoom = false;
    
    if (!m_timeWindowInitialized || width() <= 0 || height() <= 0) {
        event->accept();
        return;
    }
    
    // 🔥 ENHANCED ZOOM MODES: Support CTRL+modifier keys for different zoom modes
    Qt::KeyboardModifiers modifiers = event->modifiers();
    
    // Determine zoom mode based on modifier keys
    bool zoomTime = true;   // Default: zoom time axis
    bool zoomPrice = true;  // Default: zoom price axis
    
    if (modifiers & Qt::ControlModifier) {
        if (modifiers & Qt::ShiftModifier) {
            // CTRL+SHIFT: Lock current zoom, pan only
            zoomTime = false;
            zoomPrice = false;
        } else {
            // CTRL: Time axis zoom only (X-axis)
            zoomTime = true;
            zoomPrice = false;
        }
    } else if (modifiers & Qt::AltModifier) {
        // ALT: Price axis zoom only (Y-axis)
        zoomTime = false;
        zoomPrice = true;
    }
    // No modifiers: Default dual-axis zoom
    
    const double ZOOM_SENSITIVITY = 0.001; // Tuned for time window
    const double MIN_TIME_RANGE = 1000.0;  // 1 second minimum
    const double MAX_TIME_RANGE = 600000.0; // 10 minutes maximum
    const double MIN_PRICE_RANGE = 1.0;     // $1 minimum price range
    const double MAX_PRICE_RANGE = 10000.0; // $10k maximum price range
    
    // Calculate zoom delta
    double delta = event->angleDelta().y() * ZOOM_SENSITIVITY;
    double zoomFactor = 1.0 - delta; // Invert for natural zoom direction
    
    // Get cursor position for zoom center
    QPointF cursorPos = event->position();
    double cursorXRatio = cursorPos.x() / width();
    double cursorYRatio = 1.0 - (cursorPos.y() / height()); // Invert Y for price axis
    
    // ──── TIME AXIS ZOOM (X) ────
    if (zoomTime) {
        double currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        double newTimeRange = currentTimeRange * zoomFactor;
        newTimeRange = std::max(MIN_TIME_RANGE, std::min(MAX_TIME_RANGE, newTimeRange));
        
        if (std::abs(newTimeRange - currentTimeRange) > 10.0) {
            // Calculate cursor position in time coordinates
            int64_t cursorTimestamp = m_visibleTimeStart_ms + (cursorXRatio * currentTimeRange);
            
            // Calculate new time window bounds around cursor
            double timeDelta = newTimeRange - currentTimeRange;
            int64_t startAdjust = static_cast<int64_t>(timeDelta * cursorXRatio);
            int64_t endAdjust = static_cast<int64_t>(timeDelta * (1.0 - cursorXRatio));
            
            m_visibleTimeStart_ms -= startAdjust;
            m_visibleTimeEnd_ms += endAdjust;
        }
    }
    
    // ──── PRICE AXIS ZOOM (Y) ────
    if (zoomPrice) {
        double currentPriceRange = m_maxPrice - m_minPrice;
        double newPriceRange = currentPriceRange * zoomFactor;
        newPriceRange = std::max(MIN_PRICE_RANGE, std::min(MAX_PRICE_RANGE, newPriceRange));
        
        if (std::abs(newPriceRange - currentPriceRange) > 0.01) {
            // Calculate cursor position in price coordinates
            double cursorPrice = m_minPrice + (cursorYRatio * currentPriceRange);
            
            // Calculate new price range bounds around cursor
            double priceDelta = newPriceRange - currentPriceRange;
            double minAdjust = priceDelta * cursorYRatio;
            double maxAdjust = priceDelta * (1.0 - cursorYRatio);
            
            m_minPrice -= minAdjust;
            m_maxPrice += maxAdjust;
        }
    }
    
    emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
    
    // Disable auto-scroll on zoom
    if (m_autoScrollEnabled) {
        m_autoScrollEnabled = false;
        sLog_Camera("🔍 Auto-scroll disabled - manual zoom");
    }
    
    m_geometryDirty.store(true);
    updateDebugInfo();
    update();
    
    // 🚀 ENHANCED DEBUG: Show zoom mode and both axes info
    static int zoomDebugCount = 0;
    if (zoomDebugCount++ < 5) {
        double currentTimeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        double currentPriceRange = m_maxPrice - m_minPrice;
        
        QString zoomModeStr = "BOTH";
        if (!zoomTime && !zoomPrice) zoomModeStr = "LOCKED";
        else if (!zoomTime) zoomModeStr = "PRICE_ONLY";
        else if (!zoomPrice) zoomModeStr = "TIME_ONLY";
        
        sLog_Camera("🎥 FRACTAL ZOOM [" << zoomModeStr << "]:"
                 << "Time:" << (currentTimeRange/1000.0) << "s"
                 << "Price: $" << QString::number(currentPriceRange, 'f', 2)
                 << "Window: [" << m_visibleTimeStart_ms << "," << m_visibleTimeEnd_ms << "]"
                 << "Range: [$" << QString::number(m_minPrice, 'f', 2) 
                 << ",$" << QString::number(m_maxPrice, 'f', 2) << "]");
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
    sLog_Camera("🔍 Camera ZOOM IN: Time range:" << (newTimeRange/1000.0) << "seconds");
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
    sLog_Camera("🔍 Camera ZOOM OUT: Time range:" << (newTimeRange/1000.0) << "seconds");
}

void GPUChartWidget::resetZoom() {
    // 🔥 OPTION B: Reset to default time window
    if (m_timeWindowInitialized) {
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_visibleTimeEnd_ms = currentTime;
        m_visibleTimeStart_ms = currentTime - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
        
        // 🔥 FIX: Use dynamic price range based on recent trades
        if (m_lastTradePrice > 0) {
            updateDynamicPriceRange(m_lastTradePrice);
        } else {
            // Fallback to static range only if no trades yet
            m_minPrice = m_staticMinPrice;
            m_maxPrice = m_staticMaxPrice;
        }
        
        m_geometryDirty.store(true);
        update();
        sLog_Camera("🔄 CAMERA RESET - Time window and price range restored!");
    }
}

void GPUChartWidget::enableAutoScroll(bool enabled) {
    if (m_autoScrollEnabled != enabled) {
        m_autoScrollEnabled = enabled;
        sLog_Camera("🚀 Auto-scroll" << (enabled ? "ENABLED" : "DISABLED"));
        emit autoScrollEnabledChanged();
    }
}

void GPUChartWidget::centerOnPrice(double price) {
    // 🔥 OPTION B: Center by adjusting price range instead of pan offset
    double priceSpan = m_maxPrice - m_minPrice;
    m_minPrice = price - priceSpan / 2.0;
    m_maxPrice = price + priceSpan / 2.0;
    m_geometryDirty.store(true);
    update();
    sLog_Camera("🎯 Centered on price:" << price << "Range:" << m_minPrice << "-" << m_maxPrice);
}

void GPUChartWidget::centerOnTime(qint64 timestamp) {
    // 🔥 OPTION B: Center by adjusting time window instead of pan offset
    double timeSpan = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    m_visibleTimeStart_ms = timestamp - static_cast<int64_t>(timeSpan / 2.0);
    m_visibleTimeEnd_ms = timestamp + static_cast<int64_t>(timeSpan / 2.0);
    emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice); // 🔥 FINAL POLISH: Include price range
    m_geometryDirty.store(true);
    update();
    sLog_Camera("🎯 Centered on timestamp:" << timestamp << "Window:" << m_visibleTimeStart_ms << "-" << m_visibleTimeEnd_ms);
}

// 🎯 PHASE 4: Coordinate Transformation Methods

// 🔥 NEW: PURE STATELESS COORDINATE SYSTEM (Option B)
QPointF GPUChartWidget::worldToScreen(int64_t timestamp_ms, double price) const {
    static int logCounter = 0;
    if (logCounter++ % 500 == 0) { // Log roughly every 500 calls
        sLog_RenderDetail(QString("GPUCHART Coords: Time[%1, %2], Price[%3, %4]").arg(m_visibleTimeStart_ms).arg(m_visibleTimeEnd_ms).arg(m_minPrice).arg(m_maxPrice));
    }   

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
        // 🔥 PADDING FIX: Add 20% padding to right edge so candles aren't at the edge
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        double paddingMs = m_timeSpanMs * 0.2; // 20% padding from right edge
        m_visibleTimeEnd_ms = currentTime + static_cast<int64_t>(paddingMs);
        m_visibleTimeStart_ms = m_visibleTimeEnd_ms - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
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
    // 🔥 PADDING FIX: Initialize with padding so first candle isn't at edge
    double paddingMs = m_timeSpanMs * 0.2; // 20% padding from right edge
    m_visibleTimeEnd_ms = firstTimestamp + static_cast<int64_t>(paddingMs);
    m_visibleTimeStart_ms = m_visibleTimeEnd_ms - m_timeSpanMs;
    m_timeWindowInitialized = true;
    sLog_Chart("🔥 Initialized time window with padding:" << m_visibleTimeStart_ms << "to" << m_visibleTimeEnd_ms);
}

void GPUChartWidget::updateTimeWindow(int64_t newTimestamp) {
    if (!m_timeWindowInitialized) {
        initializeTimeWindow(newTimestamp);
        return;
    }
    
    if (m_autoScrollEnabled) {
        // 🔥 PADDING FIX: Add 20% padding to right edge for better candle visibility
        double paddingMs = m_timeSpanMs * 0.2; // 20% padding from right edge
        m_visibleTimeEnd_ms = newTimestamp + static_cast<int64_t>(paddingMs);
        m_visibleTimeStart_ms = m_visibleTimeEnd_ms - m_timeSpanMs;
        emit viewChanged(m_visibleTimeStart_ms, m_visibleTimeEnd_ms, m_minPrice, m_maxPrice);
    }
}

void GPUChartWidget::setDebugInfoVisible(bool visible) {
    if (m_debugInfoVisible != visible) {
        m_debugInfoVisible = visible;
        emit debugInfoVisibleChanged();
    }
}

void GPUChartWidget::updateDebugInfo() {
    if (!m_debugInfoVisible) {
        m_debugInfoText.clear();
        return;
    }
    
    // Calculate viewport metrics
    double timeSpanSec = (m_visibleTimeEnd_ms - m_visibleTimeStart_ms) / 1000.0;
    double priceRange = m_maxPrice - m_minPrice;
    
    // 🚀 PHASE 4: Calculate bandwidth metrics
    double totalMB = static_cast<double>(m_totalBytesUploaded.load()) / 1'000'000.0;
    int warnings = m_bandwidthWarnings.load();
    
    // Format debug text with GPU profiler metrics
    m_debugInfoText = QString(
        "📊 Frame:%1MB BW:%2MB/s Total:%3MB Warn:%4 | Points:%5"
    ).arg(m_mbPerFrame, 0, 'f', 3)
     .arg(m_mbPerFrame * 60.0, 0, 'f', 1)  // Assume 60 FPS
     .arg(totalMB, 0, 'f', 1)
     .arg(warnings)
     .arg(m_allRenderPoints.size());
    
    emit debugInfoChanged();
}