#include "zerolagheatmap.h"
#include <QSGGeometryNode>
#include <QDebug>

ZeroLagHeatmap::ZeroLagHeatmap(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    
    // Initialize viewport and LOD
    m_viewport = new UnifiedViewport(this);
    m_lod = new DynamicLOD(this);
    
    // Connect viewport changes
    connect(m_viewport, &UnifiedViewport::viewportChanged,
            this, &ZeroLagHeatmap::onViewportChanged);
    
    qDebug() << "🚀 ZeroLagHeatmap initialized";
}

void ZeroLagHeatmap::switchToTimeFrame(DynamicLOD::TimeFrame newTF) {
    if (m_currentTimeFrame == newTF) return;
    
    m_currentTimeFrame = newTF;
    m_lodDirty.store(true);
    
    qDebug() << "🎯 ZeroLagHeatmap switched to:" << DynamicLOD::getTimeFrameName(newTF);
    update(); // Trigger Qt repaint
}

void ZeroLagHeatmap::addOrderBookUpdate(int64_t timestamp_ms, 
                                       const std::vector<OrderBookLevel>& bids, 
                                       const std::vector<OrderBookLevel>& asks) {
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    // Convert order book levels to heatmap cells
    auto& activeBuffer = m_lodBuffers[static_cast<int>(m_currentTimeFrame)];
    
    // Process bids
    for (const auto& bid : bids) {
        HeatmapCell cell;
        cell.timestamp_ms = timestamp_ms;
        cell.price = bid.price;
        cell.aggregatedSize = bid.size;
        cell.intensity = std::min(1.0f, static_cast<float>(bid.size / 100.0)); // Normalize
        cell.rgba = 0x00FF0080; // Green with alpha
        
        activeBuffer.push_back(cell);
    }
    
    // Process asks
    for (const auto& ask : asks) {
        HeatmapCell cell;
        cell.timestamp_ms = timestamp_ms;
        cell.price = ask.price;
        cell.aggregatedSize = ask.size;
        cell.intensity = std::min(1.0f, static_cast<float>(ask.size / 100.0)); // Normalize
        cell.rgba = 0xFF000080; // Red with alpha
        
        activeBuffer.push_back(cell);
    }
    
    // Limit buffer size
    const size_t maxCells = 10000;
    if (activeBuffer.size() > maxCells) {
        activeBuffer.erase(activeBuffer.begin(), activeBuffer.begin() + (activeBuffer.size() - maxCells));
    }
    
    m_geometryDirty.store(true);
    update();
}

QSGNode* ZeroLagHeatmap::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) {
    Q_UNUSED(data);
    
    if (width() <= 0 || height() <= 0) {
        delete oldNode;
        return nullptr;
    }
    
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = new QSGGeometryNode();
        
        // Create geometry
        m_geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        m_geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(m_geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        
        // Create material
        m_material = new QSGVertexColorMaterial();
        m_material->setFlag(QSGMaterial::Blending);
        node->setMaterial(m_material);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    
    // Update geometry if needed
    if (m_geometryDirty.load() || m_lodDirty.load()) {
        updateGeometry();
        node->markDirty(QSGNode::DirtyGeometry);
        m_geometryDirty.store(false);
        m_lodDirty.store(false);
    }
    
    return node;
}

void ZeroLagHeatmap::updateGeometry() {
    if (!m_geometry) return;
    
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    
    const auto& activeBuffer = m_lodBuffers[static_cast<int>(m_currentTimeFrame)];
    const int verticesPerCell = 6; // 2 triangles
    int totalVertices = activeBuffer.size() * verticesPerCell;
    
    if (totalVertices == 0) {
        m_geometry->allocate(0);
        return;
    }
    
    m_geometry->allocate(totalVertices);
    auto* vertices = m_geometry->vertexDataAsColoredPoint2D();
    
    int vertexIndex = 0;
    for (const auto& cell : activeBuffer) {
        // Calculate screen position
        QPointF screenPos = worldToScreen(cell.timestamp_ms, cell.price);
        
        // Calculate cell size
        float cellWidth = 5.0f; // Fixed width for now
        float cellHeight = 2.0f + cell.intensity * 10.0f;
        
        float x1 = screenPos.x();
        float y1 = screenPos.y() - cellHeight * 0.5f;
        float x2 = x1 + cellWidth;
        float y2 = y1 + cellHeight;
        
        // Extract RGBA
        uint8_t r = (cell.rgba >> 24) & 0xFF;
        uint8_t g = (cell.rgba >> 16) & 0xFF;
        uint8_t b = (cell.rgba >> 8) & 0xFF;
        uint8_t a = cell.rgba & 0xFF;
        
        // Create rectangle (2 triangles)
        vertices[vertexIndex++].set(x1, y1, r, g, b, a);
        vertices[vertexIndex++].set(x2, y1, r, g, b, a);
        vertices[vertexIndex++].set(x1, y2, r, g, b, a);
        
        vertices[vertexIndex++].set(x2, y1, r, g, b, a);
        vertices[vertexIndex++].set(x2, y2, r, g, b, a);
        vertices[vertexIndex++].set(x1, y2, r, g, b, a);
    }
}

void ZeroLagHeatmap::onViewportChanged(const UnifiedViewport::ViewState& state) {
    // Check if LOD should change
    auto newTF = m_lod->selectOptimalTimeFrame(state.msPerPixel());
    if (newTF != m_currentTimeFrame) {
        switchToTimeFrame(newTF);
        emit timeFrameChanged(newTF, m_lod->calculateGrid(state.timeStart_ms, state.timeEnd_ms, state.pixelWidth));
    }
    
    m_geometryDirty.store(true);
    update();
}

void ZeroLagHeatmap::onTimeFrameChanged(DynamicLOD::TimeFrame newTF, const DynamicLOD::GridInfo& gridInfo) {
    Q_UNUSED(gridInfo);
    switchToTimeFrame(newTF);
}

uint32_t ZeroLagHeatmap::calculateCellColor(double intensity, bool isBid) const {
    uint8_t alpha = static_cast<uint8_t>(60 + intensity * 195);
    
    if (isBid) {
        return (0x00 << 24) | (0xFF << 16) | (0x00 << 8) | alpha; // Green
    } else {
        return (0xFF << 24) | (0x00 << 16) | (0x00 << 8) | alpha; // Red
    }
}

void ZeroLagHeatmap::rebuildInstances() {
    // TODO: Implement if needed for performance optimization
}