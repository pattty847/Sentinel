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
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>
#include <algorithm>


QSGNode* HeatmapStrategy::buildNode(const GridSliceBatch& batch) {
    if (batch.cells.empty()) return nullptr;
    
    // Create geometry node for heatmap rendering
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Calculate required vertex count (6 vertices per cell for 2 triangles)
    int cellCount = std::min(static_cast<int>(batch.cells.size()), batch.maxCells);
    int vertexCount = cellCount * 6;
    
    // Create geometry
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = batch.cells[i];
        
        // Skip cells with insufficient volume
        if (cell.liquidity < batch.minVolumeFilter) continue;
        
        // Calculate color with intensity scaling
        double scaledIntensity = calculateIntensity(cell.liquidity, batch.intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        
        float left = cell.screenRect.left();
        float right = cell.screenRect.right();
        float top = cell.screenRect.top();
        float bottom = cell.screenRect.bottom();
        
        // Triangle 1: top-left, top-right, bottom-left
        vertices[vertexIndex++].set(left, top, color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(right, top, color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(left, bottom, color.red(), color.green(), color.blue(), color.alpha());
        
        // Triangle 2: top-right, bottom-right, bottom-left
        vertices[vertexIndex++].set(right, top, color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(right, bottom, color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(left, bottom, color.red(), color.green(), color.blue(), color.alpha());
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