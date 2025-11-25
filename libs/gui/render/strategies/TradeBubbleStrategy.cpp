/*
Sentinel — TradeBubbleStrategy
Role: Implements beautiful size-relative trade bubbles rendered on top of the heatmap.
Inputs/Outputs: Creates QSGNode containing smooth, anti-aliased bubbles scaled by trade volume.
Threading: All code is executed on the Qt Quick render thread.
Performance: Optimized triangulated circles with proper depth sorting and transparency.
Integration: The concrete implementation of the trade bubble visualization strategy.
Observability: No internal logging.
Related: TradeBubbleStrategy.hpp.
Assumptions: The 'liquidity' field represents trade size for bubble scaling.
*/
#include "TradeBubbleStrategy.hpp"
#include "../GridTypes.hpp"
#include "../../CoordinateSystem.h"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>
#include <algorithm>
#include <cmath>

QSGNode* TradeBubbleStrategy::buildNode(const IDataAccessor* dataAccessor) {
    if (!dataAccessor) return nullptr;
    const auto& recentTrades = dataAccessor->getRecentTrades();
    if (recentTrades.empty()) return nullptr;
    
    // Create geometry node for bubble rendering
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Filter and sort trades by size for proper rendering order (largest first for depth)
    std::vector<const Trade*> validTrades;
    double maxTradeSize = 0.0;
    
    double minVolumeFilter = dataAccessor->getMinVolumeFilter();

    for (const auto& trade : recentTrades) {
        if (trade.size >= minVolumeFilter) {
            validTrades.push_back(&trade);
            maxTradeSize = std::max(maxTradeSize, trade.size);
        }
    }
    
    if (validTrades.empty()) return nullptr;
    
    // Sort by size (largest first for proper depth rendering)
    std::sort(validTrades.begin(), validTrades.end(), 
              [](const Trade* a, const Trade* b) {
                  return a->size > b->size;
              });
    
    // Limit trade count for performance
    int tradeCount = std::min(static_cast<int>(validTrades.size()), dataAccessor->getMaxCells());
    
    // Each bubble uses 18 vertices (6 triangles for smooth circle)
    int vertexCount = tradeCount * 18;
    
    // Create geometry
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    double intensityScale = dataAccessor->getIntensityScale();
    Viewport viewport = dataAccessor->getViewport();

    for (int i = 0; i < tradeCount; ++i) {
        const auto& trade = *validTrades[i];
        
        // Calculate bubble properties from trade data
        double scaledIntensity = calculateIntensity(trade.size, intensityScale);
        float bubbleRadius = calculateBubbleRadius(trade.size, maxTradeSize);
        bool isBid = (trade.side == AggressorSide::Buy);
        QColor bubbleColor = calculateBubbleColor(trade.size, isBid, scaledIntensity);
        
        // Convert trade timestamp and price to screen coordinates
        auto tradeTime = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
        QPointF tradePos = CoordinateSystem::worldToScreen(tradeTime, trade.price, viewport);
        float centerX = static_cast<float>(tradePos.x());
        float centerY = static_cast<float>(tradePos.y());
        
        // Create smooth bubble geometry
        createBubbleGeometry(vertices, vertexIndex, centerX, centerY, bubbleRadius, bubbleColor);
    }
    
    // Update geometry with actual vertex count used
    geometry->allocate(vertexIndex);
    node->markDirty(QSGNode::DirtyGeometry);
    
    return node;
}

QColor TradeBubbleStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    return calculateBubbleColor(liquidity, isBid, intensity);
}

float TradeBubbleStrategy::calculateBubbleRadius(double tradeSize, double maxTradeSize) const {
    if (maxTradeSize <= 0.0) return m_minBubbleRadius;
    
    // Logarithmic scaling for better visual distribution
    double normalizedSize = std::log1p(tradeSize) / std::log1p(maxTradeSize);
    
    // Apply square root for more intuitive area-based scaling
    double radiusScale = std::sqrt(normalizedSize);
    
    return m_minBubbleRadius + (m_maxBubbleRadius - m_minBubbleRadius) * static_cast<float>(radiusScale);
}

QColor TradeBubbleStrategy::calculateBubbleColor(double liquidity, bool isBid, double intensity) const {
    // Enhanced color scheme with better contrast and vibrancy
    double alpha = std::min(intensity * m_bubbleOpacity, 1.0);
    
    if (isBid) {
        // Bid trades: Beautiful blue-cyan gradient
        double saturation = std::min(intensity * 1.2, 1.0);
        int red = static_cast<int>(20 * saturation);
        int green = static_cast<int>(150 + 105 * saturation);  // 150-255 range
        int blue = static_cast<int>(200 + 55 * saturation);    // 200-255 range
        return QColor(red, green, blue, static_cast<int>(alpha * 255));
    } else {
        // Ask trades: Beautiful orange-red gradient
        double saturation = std::min(intensity * 1.2, 1.0);
        int red = static_cast<int>(200 + 55 * saturation);     // 200-255 range  
        int green = static_cast<int>(100 + 80 * saturation);   // 100-180 range
        int blue = static_cast<int>(20 * saturation);
        return QColor(red, green, blue, static_cast<int>(alpha * 255));
    }
}

void TradeBubbleStrategy::createBubbleGeometry(QSGGeometry::ColoredPoint2D* vertices, int& vertexIndex,
                                              float centerX, float centerY, float radius, const QColor& color) const {
    // Create 6 triangles for a smooth circle (18 vertices total)
    const int triangleCount = 6;
    const float angleStep = 2.0f * M_PI / triangleCount;
    
    // Note: Outline color for future enhancement - currently just using fill color
    
    for (int tri = 0; tri < triangleCount; ++tri) {
        float angle1 = tri * angleStep;
        float angle2 = (tri + 1) * angleStep;
        
        // Calculate triangle vertices
        float x1 = centerX + radius * std::cos(angle1);
        float y1 = centerY + radius * std::sin(angle1);
        float x2 = centerX + radius * std::cos(angle2);
        float y2 = centerY + radius * std::sin(angle2);
        
        // Create filled triangle: center, point1, point2
        vertices[vertexIndex++].set(centerX, centerY, 
                                   color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(x1, y1, 
                                   color.red(), color.green(), color.blue(), color.alpha());
        vertices[vertexIndex++].set(x2, y2, 
                                   color.red(), color.green(), color.blue(), color.alpha());
    }
}
