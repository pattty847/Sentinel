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