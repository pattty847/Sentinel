/*
Sentinel — HeatmapStrategy
Role: Implements the logic for rendering a liquidity heatmap from grid cell data.
Inputs/Outputs: Creates a QSGGeometryNode where each cell is a pair of colored triangles.
Threading: All code is executed on the Qt Quick render thread.
Performance: Uses a QSGVertexColorMaterial for efficient rendering of per-vertex colored geometry.
Integration: The concrete implementation of the heatmap visualization strategy.
Observability: No internal logging.
Related: HeatmapStrategy.hpp.
Assumptions: Liquidity intensity is represented by the alpha channel of the vertex color.
*/
#include "HeatmapStrategy.hpp"
#include "../GridTypes.hpp"
#include "../../../core/SentinelLogging.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <algorithm>
// #include <iostream>


QSGNode* HeatmapStrategy::buildNode(const GridSliceBatch& batch) {
    // minimal local state; avoid noisy counters
    
    if (batch.cells.empty()) {
        sLog_Render(" HEATMAP EXIT: Returning nullptr - batch is empty");
        return nullptr;
    }
    
    // reduced verbose logging
    
    // Use per-vertex colors with blending so cells have individual colors
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Calculate required vertex count (6 vertices per cell for 2 triangles)
    int total = static_cast<int>(batch.cells.size());
    int cellCount = std::min(total, batch.maxCells);
    int startIndex = std::max(0, total - cellCount); // keep newest when clipping
    int vertexCount = cellCount * 6;
    
    // Create geometry with colors
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer with colored vertices
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = batch.cells[startIndex + i];
        
        // Skip cells with insufficient volume
        if (cell.liquidity < batch.minVolumeFilter) continue;
        
        // Calculate color with intensity scaling
        double scaledIntensity = calculateIntensity(cell.liquidity, batch.intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        
        float left = cell.screenRect.left();
        float right = cell.screenRect.right();
        float top = cell.screenRect.top();
        float bottom = cell.screenRect.bottom();
        
        // Debug coordinates removed to reduce log spam
        
        // Vertex color
        const int r = color.red();
        const int g = color.green();
        const int b = color.blue();
        const int a = std::clamp(static_cast<int>(color.alpha()), 0, 255);

        // Triangle 1: top-left, top-right, bottom-left
        vertices[vertexIndex++].set(left,  top,    r, g, b, a);
        vertices[vertexIndex++].set(right, top,    r, g, b, a);
        vertices[vertexIndex++].set(left,  bottom, r, g, b, a);

        // Triangle 2: top-right, bottom-right, bottom-left
        vertices[vertexIndex++].set(right, top,    r, g, b, a);
        vertices[vertexIndex++].set(right, bottom, r, g, b, a);
        vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
    }
    
    // HEATMAP X-RANGE LOGGING
    static int frame = 0;
    if ((++frame % 30) == 0 && vertexIndex >= 6) {
        const auto* v = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
        const float firstX = v[0].x, lastX = v[vertexIndex - 1].x;
        sLog_RenderN(1, " HEATMAP X-RANGE: " << firstX << " → " << lastX << " (verts=" << vertexIndex << ")");
    }
    
    // Update geometry with actual vertex count used
    geometry->allocate(vertexIndex);
    node->markDirty(QSGNode::DirtyGeometry);
    
    // minimal completion log removed to cut spam
    
    return node;
}

QColor HeatmapStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    double alpha = std::min(intensity, 1.0); // LINUS FIX: let intensity speak
    
    if (isBid) {
        // Bid-heavy: Green spectrum
        double green = std::min(255.0 * intensity, 255.0);
        return QColor(0, static_cast<int>(green), 0, static_cast<int>(alpha * 255));
    } else {
        // Ask-heavy: Red spectrum  
        double red = std::min(255.0 * intensity, 255.0);
        return QColor(static_cast<int>(red), 0, 0, static_cast<int>(alpha * 255));
    }
}