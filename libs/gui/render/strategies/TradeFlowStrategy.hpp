/*
Sentinel — TradeFlowStrategy
Role: A concrete render strategy that visualizes individual trades on the chart.
Inputs/Outputs: Implements IRenderStrategy to turn a GridSliceBatch into a QSGNode tree.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Creates a new child QSGNode for each trade, potentially creating many nodes.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: TradeFlowStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: Each cell in the input batch represents an individual trade to be rendered.
*/
#pragma once
#include "../IRenderStrategy.hpp"

class TradeFlowStrategy : public IRenderStrategy {
public:
    TradeFlowStrategy() = default;
    ~TradeFlowStrategy() override = default;
    
    QSGNode* buildNode(const GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "TradeFlow"; }
    
private:
    struct TradeVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float radius;         // Trade dot size
    };
};