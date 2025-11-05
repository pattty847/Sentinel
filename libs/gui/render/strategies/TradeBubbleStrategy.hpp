/*
Sentinel — TradeBubbleStrategy
Role: A concrete render strategy that visualizes trades as size-relative bubbles on top of heatmap.
Inputs/Outputs: Implements IRenderStrategy to turn a GridSliceBatch into beautiful bubble QSGNodes.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Creates smooth, anti-aliased bubbles with size proportional to trade volume.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: TradeBubbleStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: Each cell in the input batch represents a trade with size used for bubble scaling.
*/
#pragma once
#include "../IRenderStrategy.hpp"

class TradeBubbleStrategy : public IRenderStrategy {
public:
    TradeBubbleStrategy() = default;
    ~TradeBubbleStrategy() override = default;
    
    QSGNode* buildNode(const GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "TradeBubbles"; }
    
    // Configuration
    void setMinBubbleRadius(float radius) { m_minBubbleRadius = radius; }
    void setMaxBubbleRadius(float radius) { m_maxBubbleRadius = radius; }
    void setBubbleOpacity(float opacity) { m_bubbleOpacity = opacity; }
    void setBubbleOutlineWidth(float width) { m_outlineWidth = width; }
    
    float getMinBubbleRadius() const { return m_minBubbleRadius; }
    float getMaxBubbleRadius() const { return m_maxBubbleRadius; }
    float getBubbleOpacity() const { return m_bubbleOpacity; }
    float getBubbleOutlineWidth() const { return m_outlineWidth; }
    
private:
    struct BubbleVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float radius;         // Bubble size
        float outline;        // Outline width for anti-aliasing
    };
    
    // Configuration parameters
    float m_minBubbleRadius = 4.0f;    // Minimum bubble size (pixels)
    float m_maxBubbleRadius = 20.0f;   // Maximum bubble size (pixels)
    float m_bubbleOpacity = 0.85f;     // Base opacity for bubbles
    float m_outlineWidth = 1.5f;       // Outline width for better visibility
    
    // Helper methods
    float calculateBubbleRadius(double tradeSize, double maxTradeSize) const;
    QColor calculateBubbleColor(double liquidity, bool isBid, double intensity) const;
    void createBubbleGeometry(QSGGeometry::ColoredPoint2D* vertices, int& vertexIndex,
                             float centerX, float centerY, float radius, const QColor& color) const;
};