/*
Sentinel — HeatmapStrategy
Role: A concrete render strategy that visualizes market liquidity as a heatmap.
Inputs/Outputs: Implements IRenderStrategy to turn a GridSliceBatch into a colored QSGNode.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Stateless. Generates six vertices per grid cell in the provided batch.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: HeatmapStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: The input batch contains valid cell data with pre-calculated screen rectangles.
*/
#pragma once
#include "../IRenderStrategy.hpp"

class HeatmapStrategy : public IRenderStrategy {
public:
    HeatmapStrategy() = default;
    ~HeatmapStrategy() override = default;
    
    QSGNode* buildNode(const GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "LiquidityHeatmap"; }
    
private:
    struct GridVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float intensity;      // Volume/liquidity intensity
    };
};