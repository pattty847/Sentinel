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
    
    for (const auto& level : bidLevels) {
        QVariantMap levelMap = level.toMap();
        double price = levelMap.value("price").toDouble();
        double size = levelMap.value("size").toDouble();
        
        if (price >= m_minPrice && price <= m_maxPrice && size > 0.0) {
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
    
    for (const auto& level : askLevels) {
        QVariantMap levelMap = level.toMap();
        double price = levelMap.value("price").toDouble();
        double size = levelMap.value("size").toDouble();
        
        if (price >= m_minPrice && price <= m_maxPrice && size > 0.0) {
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
    
    for (const auto& level : bids) {
        if (level.price >= m_minPrice && level.price <= m_maxPrice && level.size > 0.0) {
            QuadInstance quad;
            
            // 🔥 FINAL POLISH: Store raw data instead of calculating screen coordinates
            // Screen coordinates will be calculated in createQuadGeometryNode every frame
            quad.rawTimestamp = currentTimestamp;
            quad.rawPrice = level.price;
            
            // Fixed point size for heatmap squares
            const float pointSize = 4.0f;
            quad.width = pointSize;
            quad.height = pointSize;
            
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
            
            // 🔇 REDUCED DEBUG: Only log every 50th bar to reduce spam
            static int bidBarCounter = 0;
            if (++bidBarCounter % 50 == 0) {
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
    
    for (const auto& level : asks) {
        if (level.price >= m_minPrice && level.price <= m_maxPrice && level.size > 0.0) {
            QuadInstance quad;
            
            // 🔥 FINAL POLISH: Store raw data instead of calculating screen coordinates
            // Screen coordinates will be calculated in createQuadGeometryNode every frame
            quad.rawTimestamp = currentTimestamp;
            quad.rawPrice = level.price;
            
            // Fixed point size for heatmap squares
            const float pointSize = 4.0f;
            quad.width = pointSize;
            quad.height = pointSize;
            
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
            
            // 🔇 REDUCED DEBUG: Only log every 50th bar to reduce spam
            static int askBarCounter = 0;
            if (++askBarCounter % 50 == 0) {
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
    // Calculate intensity (0.0 to 1.0)
    double intensity = std::min(1.0, size * m_intensityScale / 10.0);
    
    if (isBid) {
        // Green for bids - more intense = brighter green
        return QColor::fromRgbF(0.0, 0.4 + 0.6 * intensity, 0.0, 0.3 + 0.7 * intensity);
    } else {
        // Red for asks - more intense = brighter red  
        return QColor::fromRgbF(0.4 + 0.6 * intensity, 0.0, 0.0, 0.3 + 0.7 * intensity);
    }
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
    
    // 🔥 FIXED: Use Point2D attributes to match QSGFlatColorMaterial (no per-vertex color)
    int vertexCount = instances.size() * 6;
    QSGGeometry* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    
    QSGGeometry::Point2D* vertices = geometry->vertexDataAsPoint2D();
    
    // Generate quad vertices for each instance (2 triangles = 6 vertices) - POSITION ONLY
    for (size_t i = 0; i < instances.size(); ++i) {
        const QuadInstance& quad_data = instances[i];

        // 🔥 FINAL POLISH: RECALCULATE ON EVERY FRAME, inside the rendering function
        QPointF screenPos = worldToScreen(quad_data.rawTimestamp, quad_data.rawPrice);
        
        // Use screenPos to define the quad corners
        const float pointSize = 4.0f;
        float x1 = screenPos.x() - (pointSize / 2.0f);
        float y1 = screenPos.y() - (pointSize / 2.0f);
        float x2 = x1 + pointSize;
        float y2 = y1 + pointSize;
        
        // Triangle 1: top-left, top-right, bottom-left
        vertices[i * 6 + 0].set(x1, y1);
        vertices[i * 6 + 1].set(x2, y1);
        vertices[i * 6 + 2].set(x1, y2);
        
        // Triangle 2: top-right, bottom-right, bottom-left  
        vertices[i * 6 + 3].set(x2, y1);
        vertices[i * 6 + 4].set(x2, y2);
        vertices[i * 6 + 5].set(x1, y2);
    }
    
    // Create geometry node
    QSGGeometryNode* node = new QSGGeometryNode;
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Use flat color material for performance
    QSGFlatColorMaterial* material = new QSGFlatColorMaterial;
    material->setColor(baseColor);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    return node;
} 