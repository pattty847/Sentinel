#include "heatmapinstanced.h"
#include "gpuchartwidget.h"  // For unified coordinate system
#include <QSGGeometry>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <QDebug>

using std::max;
using std::min;

HeatMapInstanced::HeatMapInstanced(QQuickItem* parent)
    : QQuickItem(parent)
{
    // Enable item for rendering
    setFlag(ItemHasContents, true);
    
    // Reserve initial capacity to avoid reallocations
    m_bidInstances.reserve(m_maxBidLevels);
    m_askInstances.reserve(m_maxAskLevels);
    
    qDebug() << "🔥 HeatmapInstanced created - Phase 2 GPU rendering ready!";
}

void HeatMapInstanced::updateBids(const QVariantList& bidLevels) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_bidInstances.clear();
    
    // 🚀 VIEWPORT BUFFER: Render order book data beyond visible range to prevent gaps
    double priceRange = m_maxPrice - m_minPrice;
    double bufferSize = priceRange * 0.5; // 50% buffer on each side
    double expandedMinPrice = m_minPrice - bufferSize;
    double expandedMaxPrice = m_maxPrice + bufferSize;
    
    for (const auto& level : bidLevels) {
        QVariantMap levelMap = level.toMap();
        double price = levelMap.value("price").toDouble();
        double size = levelMap.value("size").toDouble();
        
        // 🚀 EXPANDED RANGE: Include levels outside viewport for smooth panning
        if (price >= expandedMinPrice && price <= expandedMaxPrice && size > 0.0) {
            QuadInstance quad;
            QRectF geometry = calculateQuadGeometry(price, size);
            
            quad.x = geometry.x();
            quad.y = geometry.y();
            quad.width = geometry.width();
            quad.height = geometry.height();
            
            // Green color for bids with intensity-based alpha
            QColor color = calculateIntensityColor(size, true);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = color.alphaF();
            
            quad.intensity = size * m_intensityScale;
            quad.price = price;
            quad.size = size;
            
            m_bidInstances.push_back(quad);
        }
    }
    
    m_bidsDirty = true;
    update();
}

void HeatMapInstanced::updateAsks(const QVariantList& askLevels) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_askInstances.clear();
    
    // 🚀 VIEWPORT BUFFER: Render order book data beyond visible range to prevent gaps
    double priceRange = m_maxPrice - m_minPrice;
    double bufferSize = priceRange * 0.5; // 50% buffer on each side
    double expandedMinPrice = m_minPrice - bufferSize;
    double expandedMaxPrice = m_maxPrice + bufferSize;
    
    for (const auto& level : askLevels) {
        QVariantMap levelMap = level.toMap();
        double price = levelMap.value("price").toDouble();
        double size = levelMap.value("size").toDouble();
        
        // 🚀 EXPANDED RANGE: Include levels outside viewport for smooth panning
        if (price >= expandedMinPrice && price <= expandedMaxPrice && size > 0.0) {
            QuadInstance quad;
            QRectF geometry = calculateQuadGeometry(price, size);
            
            quad.x = geometry.x();
            quad.y = geometry.y();
            quad.width = geometry.width();
            quad.height = geometry.height();
            
            // Red color for asks with intensity-based alpha
            QColor color = calculateIntensityColor(size, false);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = color.alphaF();
            
            quad.intensity = size * m_intensityScale;
            quad.price = price;
            quad.size = size;
            
            m_askInstances.push_back(quad);
        }
    }
    
    m_asksDirty = true;
    update();
}

void HeatMapInstanced::updateOrderBook(const OrderBook& book) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // 🔥 PHASE 2: CONVERT ORDER BOOK TO GPU INSTANCES
    convertOrderBookToInstances(book);
    
    m_bidsDirty = true;
    m_asksDirty = true;
    update();
}

void HeatMapInstanced::onOrderBookUpdated(const OrderBook& book) {
    // 🔥 CRITICAL DEBUG: Confirm connection is working!
    // 🔇 REDUCED DEBUG: Only log every 50th order book to reduce spam
    static int orderBookCounter = 0;
    if (++orderBookCounter % 50 == 0) {
        qDebug() << "📊 HEATMAP: Received order book with" << book.bids.size() << "bids," << book.asks.size() << "asks";
    }
        
    updateOrderBook(book);
}

void HeatMapInstanced::clearOrderBook() {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    m_bidInstances.clear();
    m_askInstances.clear();
    
    m_bidsDirty = true;
    m_asksDirty = true;
    update();
}

void HeatMapInstanced::setMaxQuads(int maxQuads) {
    m_maxQuads = maxQuads;
    // Adjust bid/ask limits proportionally
    m_maxBidLevels = maxQuads / 2;
    m_maxAskLevels = maxQuads / 2;
    
    m_bidInstances.reserve(m_maxBidLevels);
    m_askInstances.reserve(m_maxAskLevels);
}

void HeatMapInstanced::setPriceRange(double minPrice, double maxPrice) {
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_geometryDirty = true;
    update();
}

void HeatMapInstanced::setIntensityScale(double scale) {
    m_intensityScale = scale;
    m_geometryDirty.store(true);
    update();
}

// 🔥 FINAL POLISH: Time window + price range synchronization for unified coordinates  
void HeatMapInstanced::setTimeWindow(int64_t startMs, int64_t endMs, double minPrice, double maxPrice) {
    m_visibleTimeStart_ms = startMs;
    m_visibleTimeEnd_ms = endMs;
    m_timeWindowValid = (endMs > startMs);
    
    // 🔥 FINAL POLISH: Store the synchronized price range
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    
    if (m_timeWindowValid) {
        // Trigger coordinate recalculation when time window changes
        m_geometryDirty.store(true);
        update();
        
        // 🔥 DEBUG: Verify the complete coordination is working
        static int syncCount = 0;
        if (syncCount++ < 5) {
            qDebug() << "✅🔥 COMPLETE COORDINATION ESTABLISHED:" 
                     << "HeatMap synced to GPUChart - Time:" << startMs << "to" << endMs 
                     << "ms, Price:" << minPrice << "to" << maxPrice;
        }
    }
}

void HeatMapInstanced::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    // Note: Calling base class implementation (may not exist in this Qt version)
    m_geometryDirty = true;
}

QSGNode* HeatMapInstanced::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    // 🚀 PHASE 2: TWO-DRAW-CALL ARCHITECTURE (bids + asks)
    
    auto* rootNode = static_cast<QSGTransformNode*>(oldNode);
    if (!rootNode) {
        rootNode = new QSGTransformNode;
        m_bidNode = nullptr;  // Force recreation
        m_askNode = nullptr;  // Force recreation
    }
    
    // Skip rendering if widget not properly sized
    if (width() <= 0 || height() <= 0) {
        return rootNode;
    }
    
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // 🔥 UPDATE BID GEOMETRY NODE (GREEN QUADS)
    if (m_bidsDirty || m_geometryDirty || !m_bidNode) {
        if (m_bidNode) {
            rootNode->removeChildNode(m_bidNode);
            delete m_bidNode;
        }
        
        if (!m_allBidInstances.empty()) { // 🔥 HISTORICAL HEATMAP: Use accumulated data
            m_bidNode = createQuadGeometryNode(m_allBidInstances, QColor(0, 255, 0, 180));
            rootNode->appendChildNode(m_bidNode);
        } else {
            m_bidNode = nullptr;
        }
        
        m_bidsDirty = false;
    }
    
    // 🔥 UPDATE ASK GEOMETRY NODE (RED QUADS)
    if (m_asksDirty || m_geometryDirty || !m_askNode) {
        if (m_askNode) {
            rootNode->removeChildNode(m_askNode);
            delete m_askNode;
        }
        
        if (!m_allAskInstances.empty()) { // 🔥 HISTORICAL HEATMAP: Use accumulated data
            m_askNode = createQuadGeometryNode(m_allAskInstances, QColor(255, 0, 0, 180));
            rootNode->appendChildNode(m_askNode);
        } else {
            m_askNode = nullptr;
        }
        
        m_asksDirty = false;
    }
    
    m_geometryDirty = false;
    return rootNode;
}

void HeatMapInstanced::convertOrderBookToInstances(const OrderBook& book) {
    // 🔥 HISTORICAL HEATMAP: Don't clear - we want to accumulate history!
    // m_bidInstances.clear();  // REMOVED to preserve history
    // m_askInstances.clear();  // REMOVED to preserve history
    
    // Convert bids to green quads
    convertBidsToInstances(book.bids);
    
    // Convert asks to red quads  
    convertAsksToInstances(book.asks);
    
    // 🔥 HISTORICAL HEATMAP: Sort/limit the historical data if needed
    cleanupOldHeatmapPoints(); // Clean up old points to prevent unlimited memory growth
}

void HeatMapInstanced::convertBidsToInstances(const std::vector<OrderBookLevel>& bids) {
    // Use current timestamp for this order book snapshot
    double currentTimestamp = QDateTime::currentMSecsSinceEpoch();
    
    // 🚀 EXPANDED BUFFER RANGE: Use the same buffer logic as updateBids for consistency
    double priceRange = m_maxPrice - m_minPrice;
    double bufferSize = priceRange * 0.5; // 50% buffer on each side
    double expandedMinPrice = m_minPrice - bufferSize;
    double expandedMaxPrice = m_maxPrice + bufferSize;
    
    for (const auto& level : bids) {
        // 🚀 EXPANDED RANGE: Include levels outside viewport for smooth panning
        if (level.price >= expandedMinPrice && level.price <= expandedMaxPrice && level.size > 0.0) {
            QuadInstance quad;
            
            // 🔥 FINAL POLISH: Store raw data instead of calculating screen coordinates
            // Screen coordinates will be calculated in createQuadGeometryNode every frame
            quad.rawTimestamp = currentTimestamp;
            quad.rawPrice = level.price;
            
            QColor color = calculateIntensityColor(level.size, true);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = 0.8f;
            
            quad.intensity = level.size * m_intensityScale;
            quad.price = level.price;
            quad.size = level.size;
            quad.timestamp = currentTimestamp;
            
            m_allBidInstances.push_back(quad); // 🔥 HISTORICAL HEATMAP: Accumulate all bid points
            
            // 🔇 REDUCED DEBUG: Only log every 1000th bar to reduce spam
            static int bidBarCounter = 0;
            if (++bidBarCounter % 1000 == 0) {
                qDebug() << "🟢 HISTORICAL BID POINT #" << bidBarCounter 
                         << ": Raw Price" << level.price << "Timestamp:" << quad.rawTimestamp
                         << "Total accumulated:" << m_allBidInstances.size();
            }
        }
    }
}

void HeatMapInstanced::convertAsksToInstances(const std::vector<OrderBookLevel>& asks) {
    // Use current timestamp for this order book snapshot  
    double currentTimestamp = QDateTime::currentMSecsSinceEpoch();
    
    // 🚀 EXPANDED BUFFER RANGE: Use the same buffer logic as updateAsks for consistency
    double priceRange = m_maxPrice - m_minPrice;
    double bufferSize = priceRange * 0.5; // 50% buffer on each side
    double expandedMinPrice = m_minPrice - bufferSize;
    double expandedMaxPrice = m_maxPrice + bufferSize;
    
    for (const auto& level : asks) {
        // 🚀 EXPANDED RANGE: Include levels outside viewport for smooth panning
        if (level.price >= expandedMinPrice && level.price <= expandedMaxPrice && level.size > 0.0) {
            QuadInstance quad;
            
            // 🔥 FINAL POLISH: Store raw data instead of calculating screen coordinates
            // Screen coordinates will be calculated in createQuadGeometryNode every frame
            quad.rawTimestamp = currentTimestamp;
            quad.rawPrice = level.price;
            
            QColor color = calculateIntensityColor(level.size, false);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = 0.8f;
            
            quad.intensity = level.size * m_intensityScale;
            quad.price = level.price;
            quad.size = level.size;
            quad.timestamp = currentTimestamp;
            
            m_allAskInstances.push_back(quad); // 🔥 HISTORICAL HEATMAP: Accumulate all ask points
            
            // 🔇 REDUCED DEBUG: Only log every 1000th bar to reduce spam
            static int askBarCounter = 0;
            if (++askBarCounter % 1000 == 0) {
                qDebug() << "🔴 HISTORICAL ASK POINT #" << askBarCounter 
                         << ": Raw Price" << level.price << "Timestamp:" << quad.rawTimestamp
                         << "Total accumulated:" << m_allAskInstances.size();
            }
        }
    }
}

QPointF HeatMapInstanced::worldToScreen(double timestamp_ms, double price) const {
    if (width() <= 0 || height() <= 0 || !m_timeWindowValid) {
        return QPointF(-1000, -1000); // Return off-screen point
    }

    double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    if (timeRange <= 0) timeRange = 1.0;
    double timeRatio = (timestamp_ms - m_visibleTimeStart_ms) / timeRange;
    double x = timeRatio * width();

    double priceRange = m_maxPrice - m_minPrice;
    if (priceRange <= 0) priceRange = 1.0;
    double priceRatio = (price - m_minPrice) / priceRange;
    double y = (1.0 - priceRatio) * height();

    return QPointF(x, y);
}

QRectF HeatMapInstanced::calculateQuadGeometry(double price, double size) const {
    // Map price to Y coordinate (inverse - higher price at top)
    double priceRange = m_maxPrice - m_minPrice;
    if (priceRange <= 0) priceRange = 1.0;  // Avoid division by zero
    
    double normalizedPrice = (price - m_minPrice) / priceRange;
    if (normalizedPrice < 0.0) normalizedPrice = 0.0;
    if (normalizedPrice > 1.0) normalizedPrice = 1.0;
    float y = static_cast<float>((1.0 - normalizedPrice) * height());
    
    // Map size to width (larger orders = wider quads) 
    float maxWidth = width() * 0.4f;  // Max 40% of widget width
    float minWidth = 2.0f;            // Minimum 2 pixels width
    float sizeWidth = static_cast<float>(size * m_intensityScale * 20.0);
    float quadWidth = sizeWidth;
    if (quadWidth < minWidth) quadWidth = minWidth;
    if (quadWidth > maxWidth) quadWidth = maxWidth;
    
    // Fixed height per price level (much smaller for cleaner visualization)
    float quadHeight = 3.0f;  // Fixed 3 pixel height for clean separation
    
    // Default positioning (will be adjusted per bid/ask in convert functions)
    float x = width() * 0.5f - quadWidth * 0.5f;  // Center initially
    
    return QRectF(x, y, quadWidth, quadHeight);
}

QColor HeatMapInstanced::calculateIntensityColor(double size, bool isBid) const {
    // PHASE A2: Continuous color scale like reference image
    // Calculate normalized intensity (0.0 to 1.0)
    double intensity = std::min(1.0, size * m_intensityScale / 10.0);
    
    // Power-law scaling for better visual distribution
    intensity = std::pow(intensity, 0.6);  // Makes low values more visible
    
    float r, g, b;
    
    if (intensity < 0.2) {
        // Very low: Dark blue
        r = 0.0f;
        g = 0.0f + intensity * 2.0f;  // 0.0 → 0.4
        b = 0.2f + intensity * 2.0f;  // 0.2 → 0.6
    } else if (intensity < 0.5) {
        // Low-medium: Blue → Cyan → Green
        float t = (intensity - 0.2f) / 0.3f;  // 0.0 → 1.0
        r = 0.0f;
        g = 0.4f + t * 0.4f;  // 0.4 → 0.8
        b = 0.6f - t * 0.6f;  // 0.6 → 0.0
    } else if (intensity < 0.8) {
        // Medium-high: Green → Yellow
        float t = (intensity - 0.5f) / 0.3f;  // 0.0 → 1.0
        r = t * 0.8f;         // 0.0 → 0.8
        g = 0.8f;             // Stay bright green
        b = 0.0f;
    } else {
        // High: Yellow → Orange → Red
        float t = (intensity - 0.8f) / 0.2f;  // 0.0 → 1.0
        r = 0.8f + t * 0.2f;  // 0.8 → 1.0
        g = 0.8f - t * 0.8f;  // 0.8 → 0.0
        b = 0.0f;
    }
    
    // Bid/Ask distinction: slightly shift hue
    if (isBid) {
        // Bids: Shift toward green/blue (cooler)
        g = std::min(1.0f, g * 1.1f);
        b = std::min(1.0f, b * 1.1f);
    } else {
        // Asks: Shift toward red/orange (warmer)
        r = std::min(1.0f, r * 1.1f);
    }
    
    // Alpha based on intensity (more intense = more opaque)
    float alpha = 0.4f + intensity * 0.6f;  // 0.4 → 1.0
    
    return QColor::fromRgbF(r, g, b, alpha);
}

void HeatMapInstanced::cleanupOldHeatmapPoints() {
    // 🔥 WALL OF LIQUIDITY: Manage historical points with time-based fading
    const size_t maxHistoricalPoints = static_cast<size_t>(m_maxQuads);
    double currentTime = QDateTime::currentMSecsSinceEpoch();
    const double fadeTimeMs = 30000.0; // 30 seconds fade window
    
    // 🔥 NEW: Apply time-based fading to old points before cleanup
    for (auto& quad : m_allBidInstances) {
        double age = currentTime - quad.timestamp;
        if (age > 0 && age < fadeTimeMs) {
            // Fade alpha based on age (newer = more opaque)
            float fadeFactor = 1.0f - (age / fadeTimeMs);
            quad.a = quad.a * fadeFactor * 0.8f; // Reduce base opacity for trailing effect
        }
    }
    
    for (auto& quad : m_allAskInstances) {
        double age = currentTime - quad.timestamp;
        if (age > 0 && age < fadeTimeMs) {
            // Fade alpha based on age (newer = more opaque)
            float fadeFactor = 1.0f - (age / fadeTimeMs);
            quad.a = quad.a * fadeFactor * 0.8f; // Reduce base opacity for trailing effect
        }
    }
    
    // Clean up bids if we exceed maximum (only remove oldest)
    if (m_allBidInstances.size() > maxHistoricalPoints) {
        size_t excess = m_allBidInstances.size() - maxHistoricalPoints;
        m_allBidInstances.erase(m_allBidInstances.begin(), m_allBidInstances.begin() + excess);
        qDebug() << "🌊 WAVE CLEANUP: Removed" << excess << "oldest bid points, remaining:" << m_allBidInstances.size() << "historical levels";
    }
    
    // Clean up asks if we exceed maximum (only remove oldest)
    if (m_allAskInstances.size() > maxHistoricalPoints) {
        size_t excess = m_allAskInstances.size() - maxHistoricalPoints;
        m_allAskInstances.erase(m_allAskInstances.begin(), m_allAskInstances.begin() + excess);
        qDebug() << "🌊 WAVE CLEANUP: Removed" << excess << "oldest ask points, remaining:" << m_allAskInstances.size() << "historical levels";
    }
}

void HeatMapInstanced::sortAndLimitLevels() {
    // Sort bids by price (descending - highest first)
    std::sort(m_bidInstances.begin(), m_bidInstances.end(), 
             [](const QuadInstance& a, const QuadInstance& b) {
                 return a.price > b.price;
             });
    
    // Sort asks by price (ascending - lowest first)
    std::sort(m_askInstances.begin(), m_askInstances.end(),
             [](const QuadInstance& a, const QuadInstance& b) {
                 return a.price < b.price; 
             });
    
    // Limit to max levels for performance
    if (m_bidInstances.size() > m_maxBidLevels) {
        m_bidInstances.resize(m_maxBidLevels);
    }
    
    if (m_askInstances.size() > m_maxAskLevels) {
        m_askInstances.resize(m_maxAskLevels);
    }
}

QSGGeometryNode* HeatMapInstanced::createQuadGeometryNode(const std::vector<QuadInstance>& instances, 
                                                         const QColor& baseColor) {
    if (instances.empty()) {
        return nullptr;
    }
    
    // PHASE A2: Use ColoredPoint2D to enable per-vertex gradient colors
    int vertexCount = instances.size() * 6;
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
    
    // Generate quad vertices for each instance (2 triangles = 6 vertices) with individual colors
    for (size_t i = 0; i < instances.size(); ++i) {
        const QuadInstance& quad_data = instances[i];

        // Recalculate screen position for current zoom/pan
        QPointF screenPos = worldToScreen(quad_data.rawTimestamp, quad_data.rawPrice);
        
        // PHASE A1: Adaptive screen-space sizing (larger when zoomed in to fill gaps)
        double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
        if (timeRange <= 0) timeRange = 60000.0; // Default 1 minute
        
        // Calculate zoom factor based on time window (smaller window = more zoomed in)
        double baseTimeWindow = 60000.0; // 1 minute reference
        double zoomFactor = std::max(1.0, baseTimeWindow / timeRange);
        
        // Adaptive point size: 2px minimum, scales up when zoomed in
        float adaptiveSize = std::min(8.0f, 2.0f * static_cast<float>(zoomFactor));
        
        float x1 = screenPos.x() - (adaptiveSize / 2.0f);
        float y1 = screenPos.y() - (adaptiveSize / 2.0f);
        float x2 = x1 + adaptiveSize;
        float y2 = y1 + adaptiveSize;
        
        // Convert stored RGBA to uchar for ColoredPoint2D
        unsigned char r = static_cast<unsigned char>(quad_data.r * 255);
        unsigned char g = static_cast<unsigned char>(quad_data.g * 255);
        unsigned char b = static_cast<unsigned char>(quad_data.b * 255);
        unsigned char a = static_cast<unsigned char>(quad_data.a * 255);
        
        // All 6 vertices use the same color (per-quad coloring)
        // Triangle 1: top-left, top-right, bottom-left
        vertices[i * 6 + 0].set(x1, y1, r, g, b, a);
        vertices[i * 6 + 1].set(x2, y1, r, g, b, a);
        vertices[i * 6 + 2].set(x1, y2, r, g, b, a);
        
        // Triangle 2: top-right, bottom-right, bottom-left  
        vertices[i * 6 + 3].set(x2, y1, r, g, b, a);
        vertices[i * 6 + 4].set(x2, y2, r, g, b, a);
        vertices[i * 6 + 5].set(x1, y2, r, g, b, a);
    }
    
    // Create geometry node
    QSGGeometryNode* node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Use vertex color material to respect per-vertex colors
    QSGVertexColorMaterial* material = new QSGVertexColorMaterial;
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    return node;
} 