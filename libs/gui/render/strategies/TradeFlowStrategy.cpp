/*
Sentinel — TradeFlowStrategy
Role: Implements the logic for rendering individual trades as vertical lines.
Inputs/Outputs: Creates a QSGNode containing a child node for each trade line.
Threading: All code is executed on the Qt Quick render thread.
Performance: Note: This implementation creates a large number of child nodes, which can be slow.
Integration: The concrete implementation of the trade flow visualization strategy.
Observability: No internal logging.
Related: TradeFlowStrategy.hpp.
Assumptions: The 'liquidity' field of a cell is used to determine the rendered line's length.
*/
#include "TradeFlowStrategy.hpp"
#include "../GridTypes.hpp"
#include "../../CoordinateSystem.h"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>
#include <algorithm>
#include <cmath>


QSGNode* TradeFlowStrategy::buildNode(const GridSliceBatch& batch) {
    if (batch.recentTrades.empty()) return nullptr;
    
    // Create geometry node for trade flow rendering
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Use raw trade data like TradeBubbleStrategy
    std::vector<const Trade*> validTrades;
    for (const auto& trade : batch.recentTrades) {
        if (trade.size >= batch.minVolumeFilter) {
            validTrades.push_back(&trade);
        }
    }
    
    int tradeCount = std::min(static_cast<int>(validTrades.size()), batch.maxCells);
    if (tradeCount == 0) return nullptr;
    
    int vertexCount = tradeCount * 6;
    
    // Create geometry
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    // TEMPORARY: Just return empty geometry to test if vertex processing is the crash
    // TODO: Remove this and restore the vertex processing once we confirm it's not the issue
    
    for (int i = 0; i < tradeCount && i < 10; ++i) {  // Limit to 10 trades for safety
        const auto& trade = *validTrades[i];
        
        // Add basic safety checks
        if (!validTrades[i]) continue;  // Null pointer check
        if (trade.size <= 0.0) continue;  // Invalid trade size
        
        // Calculate color and size based on trade intensity
        double scaledIntensity = calculateIntensity(trade.size, batch.intensityScale);
        bool isBid = (trade.side == AggressorSide::Buy);
        QColor color = calculateColor(trade.size, isBid, scaledIntensity);
        
        // Trade dot size based on trade size (min 2px, max 8px)
        float radius = std::max(2.0f, std::min(8.0f, static_cast<float>(scaledIntensity * 6.0)));
        
        // Convert trade timestamp and price to screen coordinates  
        auto tradeTime = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
        QPointF tradePos = CoordinateSystem::worldToScreen(tradeTime, trade.price, batch.viewport);
        float centerX = static_cast<float>(tradePos.x());
        float centerY = static_cast<float>(tradePos.y());
        
        // Safety check for coordinates
        if (!std::isfinite(centerX) || !std::isfinite(centerY)) continue;
        
        // Create triangulated circle approximation (6 triangles from center)
        for (int tri = 0; tri < 6; ++tri) {
            float angle1 = (tri * M_PI) / 3.0f;
            float angle2 = ((tri + 1) * M_PI) / 3.0f;
            
            // Triangle: center, point1, point2
            vertices[vertexIndex++].set(centerX, centerY, color.red(), color.green(), color.blue(), color.alpha());
            vertices[vertexIndex++].set(centerX + radius * cos(angle1), centerY + radius * sin(angle1), color.red(), color.green(), color.blue(), color.alpha());
            vertices[vertexIndex++].set(centerX + radius * cos(angle2), centerY + radius * sin(angle2), color.red(), color.green(), color.blue(), color.alpha());
        }
    }
    
    // Vertex count should match exactly now
    node->markDirty(QSGNode::DirtyGeometry);
    
    return node;
}

QColor TradeFlowStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    double alpha = std::min(intensity * 0.9, 1.0);
    
    if (isBid) {
        // Bid trades: Blue-green spectrum
        double blue = std::min(255.0 * intensity, 255.0); 
        double green = std::min(200.0 * intensity, 200.0);
        return QColor(0, static_cast<int>(green), static_cast<int>(blue), static_cast<int>(alpha * 255));
    } else {
        // Ask trades: Orange-red spectrum
        double red = std::min(255.0 * intensity, 255.0);
        double green = std::min(150.0 * intensity, 150.0);
        return QColor(static_cast<int>(red), static_cast<int>(green), 0, static_cast<int>(alpha * 255));
    }
}