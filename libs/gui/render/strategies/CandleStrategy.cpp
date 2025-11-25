/*
Sentinel — CandleStrategy
Role: Implements the logic for rendering OHLC candlesticks from grid cell data.
Inputs/Outputs: Creates a single QSGGeometryNode containing lines for all candle bodies and wicks.
Threading: All code is executed on the Qt Quick render thread.
Performance: Simulates thick candle bodies by drawing multiple adjacent lines.
Integration: The concrete implementation of the candlestick visualization strategy.
Observability: No internal logging.
Related: CandleStrategy.hpp.
Assumptions: Cells contain OHLC data; uses a single material for all rendered geometry.
*/
#include "CandleStrategy.hpp"
#include "../GridTypes.hpp"
#include "../../CoordinateSystem.h"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>
#include <algorithm>


QSGNode* CandleStrategy::buildNode(const IDataAccessor* dataAccessor) {
    if (!dataAccessor) return nullptr;
    auto cellsPtr = dataAccessor->getVisibleCells();
    if (!cellsPtr || cellsPtr->empty()) return nullptr;
    
    // Create geometry node for candle rendering
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Calculate required vertex count (6 vertices per candle for 2 triangles)
    const auto& cells = *cellsPtr;
    int cellCount = std::min(static_cast<int>(cells.size()), dataAccessor->getMaxCells());
    int vertexCount = cellCount * 6;
    
    // Create geometry
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    double minVolumeFilter = dataAccessor->getMinVolumeFilter();
    double intensityScale = dataAccessor->getIntensityScale();
    Viewport viewport = dataAccessor->getViewport();

    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = cells[i];
        
        // Skip cells with insufficient volume
        if (cell.liquidity < minVolumeFilter) continue;
        
        // Calculate color with intensity scaling
        double scaledIntensity = calculateIntensity(cell.liquidity, intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        
        // Convert world→screen to derive base rectangle
        QPointF topLeft = CoordinateSystem::worldToScreen(cell.timeStart_ms, cell.priceMax, viewport);
        QPointF bottomRight = CoordinateSystem::worldToScreen(cell.timeEnd_ms, cell.priceMin, viewport);
        float baseWidth = static_cast<float>(bottomRight.x() - topLeft.x());
        float baseTop = static_cast<float>(topLeft.y());
        float baseBottom = static_cast<float>(bottomRight.y());
        float centerX = static_cast<float>((topLeft.x() + bottomRight.x()) * 0.5);
        float volumeWidth = baseWidth * std::min(1.0f, static_cast<float>(scaledIntensity * 0.8));
        float left = centerX - volumeWidth * 0.5f;
        float right = centerX + volumeWidth * 0.5f;
        float top = baseTop;
        float bottom = baseBottom;
        
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

QColor CandleStrategy::getBullishColor(double intensity) const {
    Q_UNUSED(intensity)  // Reserved for future customization
    return QColor(0, 255, 0, 255);  // Solid green
}

QColor CandleStrategy::getBearishColor(double intensity) const {
    Q_UNUSED(intensity)  // Reserved for future customization
    return QColor(255, 0, 0, 255);  // Solid red
}
