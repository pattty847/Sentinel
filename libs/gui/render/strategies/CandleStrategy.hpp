#pragma once
#include "../IRenderStrategy.hpp"

class CandleStrategy : public IRenderStrategy {
public:
    CandleStrategy() = default;
    ~CandleStrategy() override = default;
    
    QSGNode* buildNode(const GridSliceBatch& batch) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "VolumeCandles"; }
    
private:
    struct CandleVertex {
        float x, y;           // Position
        float r, g, b, a;     // Color
        float width;          // Candle width
    };
};