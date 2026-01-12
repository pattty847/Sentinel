/*
Sentinel — TradeFlowStrategy
Role: A concrete render strategy that visualizes individual trades on the chart.
Inputs/Outputs: Implements IRenderStrategy to turn accessor-provided trade data into a QSGNode tree.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Creates a new child QSGNode for each trade, potentially creating many nodes.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: TradeFlowStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: The accessor provides individual trades to be rendered.
*/
#pragma once
#include "../IRenderStrategy.hpp"

class TradeFlowStrategy : public IRenderStrategy {
public:
    TradeFlowStrategy() = default;
    ~TradeFlowStrategy() override = default;
    
    QSGNode* buildNode(const IDataAccessor* dataAccessor) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "TradeFlow"; }
    
private:
    struct TradeVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float radius;         // Trade dot size
    };
};