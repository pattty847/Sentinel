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

// 🔥 NEW: Time window synchronization for unified coordinates
void HeatMapInstanced::setTimeWindow(int64_t startMs, int64_t endMs) {
    m_visibleTimeStart_ms = startMs;
    m_visibleTimeEnd_ms = endMs;
    m_timeWindowValid = (endMs > startMs);
    
    if (m_timeWindowValid) {
        // Trigger coordinate recalculation when time window changes
        m_geometryDirty.store(true);
        update();
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
        
        if (!m_bidInstances.empty()) {
            m_bidNode = createQuadGeometryNode(m_bidInstances, QColor(0, 255, 0, 180));
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
        
        if (!m_askInstances.empty()) {
            m_askNode = createQuadGeometryNode(m_askInstances, QColor(255, 0, 0, 180));
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
    // Clear existing instances
    m_bidInstances.clear();
    m_askInstances.clear();
    
    // Convert bids to green quads
    convertBidsToInstances(book.bids);
    
    // Convert asks to red quads  
    convertAsksToInstances(book.asks);
    
    // Sort and limit for performance
    sortAndLimitLevels();
}

void HeatMapInstanced::convertBidsToInstances(const std::vector<OrderBookLevel>& bids) {
    // Use current timestamp for this order book snapshot
    double currentTimestamp = QDateTime::currentMSecsSinceEpoch();
    
    for (const auto& level : bids) {
        if (level.price >= m_minPrice && level.price <= m_maxPrice && level.size > 0.0) {
            QuadInstance quad;
            
            // 🚀 UNIFIED TIME-SERIES COORDINATE MAPPING (Option B approach)
            QPointF basePos;
            if (m_timeWindowValid) {
                // Use same coordinate system as trades
                double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
                double timeRatio = (currentTimestamp - m_visibleTimeStart_ms) / timeRange;
                double x = timeRatio * width();
                
                // Map price to Y coordinate (same as trades)
                double priceRange = m_maxPrice - m_minPrice;
                double priceRatio = (level.price - m_minPrice) / priceRange;
                double y = (1.0 - priceRatio) * height();
                
                basePos = QPointF(x, y);
            } else {
                // Fallback: place at 80% of screen width
                double x = 0.8 * width();
                double priceRange = m_maxPrice - m_minPrice;
                double priceRatio = (level.price - m_minPrice) / priceRange;
                double y = (1.0 - priceRatio) * height();
                basePos = QPointF(x, y);
            }
            
            // 🔥 ORDER BOOK POSITIONING: Bids extend RIGHT from the time column
            float volumeScale = std::min(1.0f, static_cast<float>(level.size * 100.0f));
            float bidWidth = width() * 0.2f * volumeScale;  // Max 20% of screen width
            
            quad.x = basePos.x();  // Start at the time column
            quad.y = basePos.y();
            quad.width = bidWidth;  // Extend rightward
            quad.height = 4.0f;     // Thin horizontal bars
            
            QColor color = calculateIntensityColor(level.size, true);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = 0.8f;
            
            quad.intensity = level.size * m_intensityScale;
            quad.price = level.price;
            quad.size = level.size;
            quad.timestamp = currentTimestamp;
            
            m_bidInstances.push_back(quad);
            
            // 🔇 REDUCED DEBUG: Only log every 50th bar to reduce spam
            static int bidBarCounter = 0;
            if (++bidBarCounter % 50 == 0) {
                qDebug() << "🟢 UNIFIED BID BAR #" << bidBarCounter 
                         << ": Price" << level.price << "→ ColumnX:" << basePos.x() << "Y:" << quad.y;
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
            
            // 🚀 UNIFIED TIME-SERIES COORDINATE MAPPING (Option B approach)
            QPointF basePos;
            if (m_timeWindowValid) {
                // Use same coordinate system as trades
                double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
                double timeRatio = (currentTimestamp - m_visibleTimeStart_ms) / timeRange;
                double x = timeRatio * width();
                
                // Map price to Y coordinate (same as trades)
                double priceRange = m_maxPrice - m_minPrice;
                double priceRatio = (level.price - m_minPrice) / priceRange;
                double y = (1.0 - priceRatio) * height();
                
                basePos = QPointF(x, y);
            } else {
                // Fallback: place at 80% of screen width
                double x = 0.8 * width();
                double priceRange = m_maxPrice - m_minPrice;
                double priceRatio = (level.price - m_minPrice) / priceRange;
                double y = (1.0 - priceRatio) * height();
                basePos = QPointF(x, y);
            }
            
            // 🔥 ORDER BOOK POSITIONING: Asks extend LEFT from the time column
            float volumeScale = std::min(1.0f, static_cast<float>(level.size * 100.0f));
            float askWidth = width() * 0.2f * volumeScale;  // Max 20% of screen width
            
            quad.x = basePos.x() - askWidth;  // Start LEFT of the time column
            quad.y = basePos.y();
            quad.width = askWidth;  // Extend leftward
            quad.height = 4.0f;     // Thin horizontal bars
            
            QColor color = calculateIntensityColor(level.size, false);
            quad.r = color.redF();
            quad.g = color.greenF();
            quad.b = color.blueF();
            quad.a = 0.8f;
            
            quad.intensity = level.size * m_intensityScale;
            quad.price = level.price;
            quad.size = level.size;
            quad.timestamp = currentTimestamp;
            
            m_askInstances.push_back(quad);
            
            // 🔇 REDUCED DEBUG: Only log every 50th bar to reduce spam
            static int askBarCounter = 0;
            if (++askBarCounter % 50 == 0) {
                qDebug() << "🔴 UNIFIED ASK BAR #" << askBarCounter 
                         << ": Price" << level.price << "→ ColumnX:" << basePos.x() << "Y:" << quad.y;
            }
        }
    }
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
        const QuadInstance& quad = instances[i];
        
        // Quad corners
        float x1 = quad.x;
        float y1 = quad.y;
        float x2 = quad.x + quad.width;
        float y2 = quad.y + quad.height;
        
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