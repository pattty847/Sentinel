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
#include "../../CoordinateSystem.h"
#include "../../../core/SentinelLogging.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <algorithm>
// #include <iostream>


QSGNode* HeatmapStrategy::buildNode(const GridSliceBatch& batch) {

    /*
        Build a GPU scene graph node for rendering the heatmap. 
        Take our CellInstance data and convert them to colored triangles in world space.
    */

    if (batch.cells.empty()) {
        sLog_Render(" HEATMAP EXIT: Returning nullptr - batch is empty");
        return nullptr;
    }
    
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
    
    // Fill vertex buffer with colored vertices (world→screen per cell)
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = batch.cells[startIndex + i];
        if (cell.liquidity < batch.minVolumeFilter) continue;

        // Color with intensity scaling
        double scaledIntensity = calculateIntensity(cell.liquidity, batch.intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        const int r = color.red();
        const int g = color.green();
        const int b = color.blue();
        const int a = std::clamp(static_cast<int>(color.alpha()), 0, 255);

        // Convert world→screen using batch.viewport
        QPointF topLeft = CoordinateSystem::worldToScreen(cell.timeStart_ms, cell.priceMax, batch.viewport);
        QPointF bottomRight = CoordinateSystem::worldToScreen(cell.timeEnd_ms, cell.priceMin, batch.viewport);

        const float left = static_cast<float>(topLeft.x());
        const float top = static_cast<float>(topLeft.y());
        const float right = static_cast<float>(bottomRight.x());
        const float bottom = static_cast<float>(bottomRight.y());

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