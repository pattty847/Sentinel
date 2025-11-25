/*
Sentinel — HeatmapStrategy
Role: A concrete render strategy that visualizes market liquidity as a heatmap.
Inputs/Outputs: Implements IRenderStrategy to turn accessor-provided cell data into a colored QSGNode.
Threading: Methods are called exclusively on the Qt Quick render thread.
Performance: DOD: inline color calc, node reuse. Proven chunking for Windows/ANGLE 65k limit.
Integration: Instantiated and managed by UnifiedGridRenderer as a pluggable strategy.
Observability: No internal logging.
Related: HeatmapStrategy.cpp, IRenderStrategy.hpp, UnifiedGridRenderer.h, GridTypes.hpp.
Assumptions: The accessor provides valid cell data and viewport for coordinate conversion.
*/
#pragma once
#include "../IRenderStrategy.hpp"

class HeatmapStrategy : public IRenderStrategy {
public:
    HeatmapStrategy() = default;
    ~HeatmapStrategy() override = default;
    
    QSGNode* buildNode(const IDataAccessor* dataAccessor) override;
    bool updateNode(QSGNode* existingNode, const IDataAccessor* dataAccessor) override;
    QColor calculateColor(double liquidity, bool isBid, double intensity) const override;
    const char* getStrategyName() const override { return "LiquidityHeatmap"; }
    
private:
    // Windows/ANGLE enforces 16-bit index limits - keep under 65535 vertices per node
    static constexpr int kMaxVerticesPerNode = 60000; // safety margin
    static constexpr int kVertsPerCell = 6;
    static constexpr int kCellsPerChunk = kMaxVerticesPerNode / kVertsPerCell;
    
    // DOD: Inline color calculation (no QColor allocation per cell)
    void calculateColorInline(double liquidity, bool isBid, double intensity,
                              int& r, int& g, int& b, int& a) const;
};
