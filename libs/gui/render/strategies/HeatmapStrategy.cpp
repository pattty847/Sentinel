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
#include <iostream>


QSGNode* HeatmapStrategy::buildNode(const GridSliceBatch& batch) {
    // 🚨 PROOF: Add direct output that can't be cached or filtered
    static int renderCounter = 0;
    qDebug() << "🚨 HEATMAP buildNode() DEFINITELY EXECUTING - Counter:" << ++renderCounter;
    std::cout << "🚨 DIRECT STDOUT: HeatmapStrategy::buildNode() called with " << batch.cells.size() << " cells!" << std::endl;
    
    sLog_Render("🎯 HEATMAP ENTRY #" << renderCounter << ": buildNode called with " 
                << batch.cells.size() << " cells");
    
    if (batch.cells.empty()) {
        sLog_Render("🎯 HEATMAP EXIT: Returning nullptr - batch is empty");
        return nullptr;
    }
    
    sLog_Render("🎯 HEATMAP RENDER #" << renderCounter << ": Building geometry with " 
                << batch.cells.size() << " cells, maxCells=" << batch.maxCells 
                << " minVolumeFilter=" << batch.minVolumeFilter);
    
    // 🔍 DEBUGGING: Try solid color material instead of vertex color
    auto* node = new QSGGeometryNode;
    auto* material = new QSGFlatColorMaterial;
    material->setColor(QColor(255, 0, 0, 128)); // Solid red for visibility
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Calculate required vertex count (6 vertices per cell for 2 triangles)
    int cellCount = std::min(static_cast<int>(batch.cells.size()), batch.maxCells);
    int vertexCount = cellCount * 6;
    
    // Create geometry for flat color (simple Point2D instead of ColoredPoint2D)
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer (simple Point2D coordinates)
    auto* vertices = static_cast<QSGGeometry::Point2D*>(geometry->vertexData());
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
        
        // Debug coordinates removed to reduce log spam
        
        // Triangle 1: top-left, top-right, bottom-left (simple coordinates, no color)
        vertices[vertexIndex++].set(left, top);
        vertices[vertexIndex++].set(right, top);
        vertices[vertexIndex++].set(left, bottom);
        
        // Triangle 2: top-right, bottom-right, bottom-left
        vertices[vertexIndex++].set(right, top);
        vertices[vertexIndex++].set(right, bottom);
        vertices[vertexIndex++].set(left, bottom);
    }
    
    // Update geometry with actual vertex count used
    geometry->allocate(vertexIndex);
    node->markDirty(QSGNode::DirtyGeometry);
    
    sLog_Render("🎯 HEATMAP COMPLETE: Created geometry with " << vertexIndex << " vertices from " 
                << batch.cells.size() << " cells (after volume filtering)");
    
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