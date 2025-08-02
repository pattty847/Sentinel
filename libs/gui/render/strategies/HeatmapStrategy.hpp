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