#include "heatmapinstanced.h"
#include "SentinelLogging.hpp"

#include <QSGGeometry>
#include <QSGVertexColorMaterial>
#include <QColor>
#include <algorithm>
#include <cmath>
#include <QDebug>

using std::max;
using std::min;

HeatmapBatched::HeatmapBatched(QQuickItem* parent)
    : QQuickItem(parent)
    , m_activeTimeFrame(CandleLOD::TF_1sec)
    , m_geometryDirty(false)
    , m_bidsDirty(false)
    , m_asksDirty(false)
    , m_timeWindowValid(false)
    , m_intensityScale(1.0)
    , m_bidNode(nullptr)
    , m_askNode(nullptr)
{
    setFlag(ItemHasContents, true);
    sLog_Init("HeatmapBatched: Model B architecture initialized");
}

HeatmapBatched::~HeatmapBatched()
{
    // Clean up GPU resources
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    if (m_bidNode) {
        delete m_bidNode;
        m_bidNode = nullptr;
    }
    
    if (m_askNode) {
        delete m_askNode;
        m_askNode = nullptr;
    }
    
    // Clear all buffers
    for (auto& buffer : m_bidBuffers) {
        buffer.clear();
    }
    for (auto& buffer : m_askBuffers) {
        buffer.clear();
    }
    
    sLog_Init("HeatmapBatched: Model B resources cleaned up");
}

void HeatmapBatched::onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame) {
    if (m_activeTimeFrame != newTimeFrame) {
        sLog_Chart("HEATMAP: Switching to timeframe " << static_cast<int>(newTimeFrame));
        m_activeTimeFrame = newTimeFrame;
        m_geometryDirty = true;
        update();
    }
}

void HeatmapBatched::onHeatmapDataReady(CandleLOD::TimeFrame timeframe, const std::vector<QuadInstance>& bids, const std::vector<QuadInstance>& asks) {
    std::lock_guard<std::mutex> lock(m_dataMutex);

    // --- Create the historical trail by APPENDING data, not replacing it ---
    auto& bidBuffer = m_bidBuffers[timeframe];
    bidBuffer.insert(bidBuffer.end(), bids.begin(), bids.end());

    auto& askBuffer = m_askBuffers[timeframe];
    askBuffer.insert(askBuffer.end(), asks.begin(), asks.end());

    // --- Manage memory: If a buffer gets too big, remove the oldest points ---
    const size_t maxPointsPerTimeframe = 50000; // Keep the last 50k points for each timeframe
    if (bidBuffer.size() > maxPointsPerTimeframe) {
        bidBuffer.erase(bidBuffer.begin(), bidBuffer.begin() + (bidBuffer.size() - maxPointsPerTimeframe));
    }
    if (askBuffer.size() > maxPointsPerTimeframe) {
        askBuffer.erase(askBuffer.begin(), askBuffer.begin() + (askBuffer.size() - maxPointsPerTimeframe));
    }

    // If the data that just arrived is for our currently active view, mark it for redraw
    if (timeframe == m_activeTimeFrame) {
        m_bidsDirty = true;
        m_asksDirty = true;
        update();
        emit dataUpdated();
    }
}

void HeatmapBatched::setPriceRange(double minPrice, double maxPrice) {
    if (minPrice >= maxPrice) {
        sLog_Error("HEATMAP: Invalid price range [" << minPrice << ", " << maxPrice << "]");
        return;
    }
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_geometryDirty = true;
    update();
}

void HeatmapBatched::setIntensityScale(double scale) {
    if (scale <= 0) {
        sLog_Error("HEATMAP: Invalid intensity scale: " << scale);
        return;
    }
    m_intensityScale = scale;
    m_geometryDirty = true;
    update();
}

// Add these new functions anywhere in the file

void HeatmapBatched::setViewTimeStart(qint64 time) {
    if (m_visibleTimeStart_ms != time) {
        static int logCount = 0;
        if (++logCount <= 3) { // Only log first 3 calls
            sLog_Chart("🔥 HEATMAP: setViewTimeStart called with:" << time << "(was:" << m_visibleTimeStart_ms << ")");
        }
        m_visibleTimeStart_ms = time;
        m_geometryDirty = true; // Mark that geometry needs recalculating
        m_timeWindowValid = (m_visibleTimeEnd_ms > m_visibleTimeStart_ms);
        update(); // Schedule a repaint
        emit viewChanged();
    }
}

void HeatmapBatched::setViewTimeEnd(qint64 time) {
    if (m_visibleTimeEnd_ms != time) {
        static int logCount = 0;
        if (++logCount <= 3) { // Only log first 3 calls
            sLog_Chart("🔥 HEATMAP: setViewTimeEnd called with:" << time << "(was:" << m_visibleTimeEnd_ms << ")");
        }
        m_visibleTimeEnd_ms = time;
        m_geometryDirty = true;
        m_timeWindowValid = (m_visibleTimeEnd_ms > m_visibleTimeStart_ms);
        update();
        emit viewChanged();
    }
}

void HeatmapBatched::setViewMinPrice(double price) {
    if (m_minPrice != price) {
        static int logCount = 0;
        if (++logCount <= 3) { // Only log first 3 calls
            sLog_Chart("🔥 HEATMAP: setViewMinPrice called with:" << price << "(was:" << m_minPrice << ")");
        }
        m_minPrice = price;
        m_geometryDirty = true;
        update();
        emit viewChanged();
    }
}

void HeatmapBatched::setViewMaxPrice(double price) {
    if (m_maxPrice != price) {
        static int logCount = 0;
        if (++logCount <= 3) { // Only log first 3 calls
            sLog_Chart("🔥 HEATMAP: setViewMaxPrice called with:" << price << "(was:" << m_maxPrice << ")");
        }
        m_maxPrice = price;
        m_geometryDirty = true;
        update();
        emit viewChanged();
    }
}

void HeatmapBatched::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) {
    m_geometryDirty = true;
    update();
}

QSGNode* HeatmapBatched::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    if (width() <= 0 || height() <= 0 || !m_timeWindowValid) {
        // If the component isn't visible or ready, clean up and do nothing.
        if (oldNode) {
            delete oldNode;
            m_bidNode = nullptr;
            m_askNode = nullptr;
        }
        return nullptr;
    }

    QSGTransformNode* rootNode = static_cast<QSGTransformNode*>(oldNode);

    // ================== CREATE ONCE ==================
    // If the root node doesn't exist, this is our first time running.
    // Create the entire node structure ONCE.
    if (!rootNode) {
        rootNode = new QSGTransformNode();

        // Create the bid node. It will be empty at first, but it now
        // permanently exists in the scene graph.
        m_bidNode = new QSGGeometryNode;
        auto* bidGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        bidGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        m_bidNode->setGeometry(bidGeom);
        m_bidNode->setMaterial(new QSGVertexColorMaterial());
        m_bidNode->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
        rootNode->appendChildNode(m_bidNode);

        // Create the ask node.
        m_askNode = new QSGGeometryNode;
        auto* askGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        askGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        m_askNode->setGeometry(askGeom);
        m_askNode->setMaterial(new QSGVertexColorMaterial());
        m_askNode->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
        rootNode->appendChildNode(m_askNode);
        
        // Mark everything as dirty to force an initial data load.
        m_geometryDirty = true;
        m_bidsDirty = true;
        m_asksDirty = true;
    }

    // ================== UPDATE FOREVER ==================
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    try {
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // If bids data or view has changed, update the bid node's GEOMETRY.
        if (m_bidsDirty || m_geometryDirty) {
            const auto& activeBids = m_bidBuffers[m_activeTimeFrame];
            // We pass the EXISTING node to be updated, not creating a new one.
            updateNodeGeometry(m_bidNode, activeBids, QColor(0, 255, 0, 180));
        }

        // If asks data or view has changed, update the ask node's GEOMETRY.
        if (m_asksDirty || m_geometryDirty) {
            const auto& activeAsks = m_askBuffers[m_activeTimeFrame];
            updateNodeGeometry(m_askNode, activeAsks, QColor(255, 0, 0, 180));
        }

        // Reset all dirty flags for the next frame.
        m_bidsDirty = false;
        m_asksDirty = false;
        m_geometryDirty = false;
        
        // Log performance metrics
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        if (duration.count() > 1000) { // Log if render takes > 1ms
            sLog_Chart("HEATMAP: Render time: " << duration.count() << "µs for " 
                      << m_bidBuffers[m_activeTimeFrame].size() + m_askBuffers[m_activeTimeFrame].size() << " quads");
        }
        
    } catch (const std::exception& e) {
        sLog_Error("HEATMAP: Render failed: " << e.what());
    }

    return rootNode;
}

QPointF HeatmapBatched::worldToScreen(qint64 timestamp_ms, double price) const {
    if (!m_timeWindowValid || width() <= 0 || height() <= 0) {
        return QPointF(-1000, -1000);
    }

    double timeRange = m_visibleTimeEnd_ms - m_visibleTimeStart_ms;
    double priceRange = m_maxPrice - m_minPrice;

    static int logCounter = 0;
    if (++logCounter <= 5) { // Only log first 5 calls
        sLog_RenderDetail(QString("HEATMAP: WorldToScreen: Timestamp: %1, TimeEnd: %2, TimeStart: %3, PriceRange: %4, MinPrice: %5, MaxPrice: %6").arg(timestamp_ms).arg(m_visibleTimeEnd_ms).arg(m_visibleTimeStart_ms).arg(priceRange).arg(m_minPrice).arg(m_maxPrice));
    }
    
    if (timeRange <= 0) {
        return QPointF(-1000, -1000);
    }
    
    // 🔥 FIX: Add bounds checking and clamp price ratio
    if (priceRange <= 0) priceRange = 1.0;
    double timeRatio = (timestamp_ms - m_visibleTimeStart_ms) / timeRange;
    double rawPriceRatio = (price - m_minPrice) / priceRange;
    double priceRatio = max(-0.1, min(1.1, rawPriceRatio)); // Manual clamp
    double y = (1.0 - priceRatio) * height();
    
    // 🔥 DEBUG: Log coordinate calculation for first few calls
    static int coordDebugCount = 0;
    if (++coordDebugCount <= 5) {
        sLog_RenderDetail("🔧 HEATMAP COORD FIX: price=" << price 
                     << "minPrice=" << m_minPrice << "maxPrice=" << m_maxPrice
                     << "y=" << y << "height=" << height());
    }
    
    return QPointF(timeRatio * width(), y);
}

// This replaces your old createQuadGeometryNode function.
void HeatmapBatched::updateNodeGeometry(QSGGeometryNode* node,
                                      const std::vector<QuadInstance>& instances,
                                      const QColor& baseColor)
{
    if (!node) return; // Safety check

    QSGGeometry* geometry = node->geometry();
    const int verticesPerQuad = 6;
    int requiredVertices = instances.size() * verticesPerQuad;

    // 🔥 DEBUG: Log geometry creation
    static int geometryDebugCount = 0;
    if (++geometryDebugCount <= 5) {
        sLog_RenderDetail("🔥 HEATMAP: updateNodeGeometry called with" << instances.size() << "instances, required vertices:" << requiredVertices);
    }

    // Reallocate the buffer of the existing geometry if needed.
    if (geometry->vertexCount() != requiredVertices) {
        geometry->allocate(requiredVertices);
    }

    if (requiredVertices == 0) {
        // If there's no data, mark the geometry as changed to clear it.
        node->markDirty(QSGNode::DirtyGeometry);
        return;
    }

    QSGGeometry::ColoredPoint2D* vertices = geometry->vertexDataAsColoredPoint2D();
    int vertex_count = 0;

    // Use the finest-resolution bucket duration to determine the base width of our heatmap bars.
    double timePerBucket = std::max<double>(
        CandleLOD::getTimeFrameDuration(m_activeTimeFrame), // Use current timeframe
        2000.0 // Minimum 2 second width to fill gaps
    );

    // Find the max aggregated size in this batch to normalize intensity against.
    // This makes the colors and widths relative to the current visible data.
    double max_size = 0.0;
    for (const auto& instance : instances) {
        if (instance.size > max_size) {
            max_size = instance.size;
        }
    }
    if (max_size == 0.0) max_size = 1.0; // Avoid division by zero

    for (const auto& instance : instances) {
        // --- 1. Calculate Volume-Based Intensity (0.0 to 1.0) ---
        double intensity = std::pow(instance.size / max_size, 0.75); // Use pow() for better visual curve

        // THE FIX IS HERE:
        // On every render, recalculate the screen position using the
        // heatmap's (now perfectly synced) member variables for the view window.
        // This is the exact same logic GPUChartWidget uses.
        QPointF startPoint = worldToScreen(instance.rawTimestamp, instance.rawPrice);
        QPointF endPoint = worldToScreen(instance.rawTimestamp + timePerBucket, instance.rawPrice);
        
        float x1 = startPoint.x();
        float x2 = std::max(x1 + 1.0f, static_cast<float>(endPoint.x())); // Ensure at least 1px wide
        float y = startPoint.y();

        // Make the bar wider for higher volume
        float barHeight = 8.0f + (intensity * 20.0f); // 8px base, up to 28px tall for high volume
        float y1 = y - (barHeight / 2.0f);
        float y2 = y + (barHeight / 2.0f);

        // 🔥 DEBUG: Log first few bar dimensions
        static int barDebugCount = 0;
        if (barDebugCount++ < 3) {
            sLog_RenderDetail("🔥 HEATMAP BAR DEBUG #" << barDebugCount << ":"
                         << "Intensity:" << intensity
                         << "BarHeight:" << barHeight
                         << "Position:(" << x1 << "," << y1 << ") to (" << x2 << "," << y2 << ")"
                         << "Screen coords:(" << startPoint.x() << "," << startPoint.y() << ")");
        }

        // --- 3. Calculate Volume-Based Color & Opacity ---
        unsigned char r = baseColor.red();
        unsigned char g = baseColor.green();
        unsigned char b = baseColor.blue();
        // More volume = more opaque
        unsigned char a = static_cast<unsigned char>(60 + intensity * 195); // Alpha from ~60 to 255

        // --- 4. Create the 2 Triangles for the Rectangle ---
        // Triangle 1
        vertices[vertex_count++].set(x1, y1, r, g, b, a);
        vertices[vertex_count++].set(x2, y1, r, g, b, a);
        vertices[vertex_count++].set(x1, y2, r, g, b, a);

        // Triangle 2
        vertices[vertex_count++].set(x2, y1, r, g, b, a);
        vertices[vertex_count++].set(x2, y2, r, g, b, a);
        vertices[vertex_count++].set(x1, y2, r, g, b, a);
    }
    
    // IMPORTANT: After filling the vertices, mark the node as dirty so the
    // scene graph knows to re-upload this new data to the GPU.
    node->markDirty(QSGNode::DirtyGeometry);
}