/*
Sentinel — CandleStrategy
Role: A concrete render strategy that visualizes market data as OHLC candlesticks.
Inputs/Outputs: Implements IRenderStrategy to turn a GridSliceBatch into a QSGNode of lines.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: Renders all geometry into a single QSGNode for efficiency.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: CandleStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: Input cells contain OHLC data in their 'customData' map.
*/
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