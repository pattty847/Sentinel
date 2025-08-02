#include "CandleStrategy.hpp"
#include "../GridTypes.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>
#include <algorithm>


QSGNode* CandleStrategy::buildNode(const GridSliceBatch& batch) {
    if (batch.cells.empty()) return nullptr;
    
    // Create geometry node for candle rendering
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Calculate required vertex count (6 vertices per candle for 2 triangles)
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
        
        // Volume-weighted candle width (80% of cell width based on intensity)
        float baseWidth = cell.screenRect.width();
        float volumeWidth = baseWidth * std::min(1.0f, static_cast<float>(scaledIntensity * 0.8));
        
        float centerX = cell.screenRect.center().x();
        float left = centerX - volumeWidth * 0.5f;
        float right = centerX + volumeWidth * 0.5f;
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

QColor CandleStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    double alpha = std::min(intensity * 0.85, 1.0);
    
    if (isBid) {
        // Bullish candles: Green spectrum with yellow highlights
        double green = std::min(255.0 * intensity, 255.0);
        double yellow = std::min(100.0 * intensity, 100.0);
        return QColor(static_cast<int>(yellow), static_cast<int>(green), 0, static_cast<int>(alpha * 255));
    } else {
        // Bearish candles: Red spectrum with orange highlights
        double red = std::min(255.0 * intensity, 255.0);
        double orange = std::min(80.0 * intensity, 80.0);
        return QColor(static_cast<int>(red), static_cast<int>(orange), 0, static_cast<int>(alpha * 255));
    }
}