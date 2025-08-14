/*
Sentinel — IRenderStrategy
Role: Defines the abstract interface for all rendering strategies.
Inputs/Outputs: Defines the contract for turning a GridSliceBatch into a renderable QSGNode.
Threading: Methods are designed to be called on the Qt Quick render thread.
Performance: Interface is designed for batch operations to optimize rendering performance.
Integration: Implemented by concrete strategies (e.g., HeatmapStrategy) and used by UnifiedGridRenderer.
Observability: No diagnostics defined; responsibility of the concrete implementation.
Related: UnifiedGridRenderer.h, GridTypes.hpp, HeatmapStrategy.hpp, TradeFlowStrategy.hpp.
Assumptions: Implementations will be managed and invoked by UnifiedGridRenderer.
*/
#pragma once
#include <QSGNode>

class QSGGeometryNode;

// Forward declarations
struct CellInstance;
struct GridSliceBatch;

class IRenderStrategy {
public:
    virtual ~IRenderStrategy() = default;
    
    virtual QSGNode* buildNode(const GridSliceBatch& batch) = 0;
    virtual QColor calculateColor(double liquidity, bool isBid, double intensity) const = 0;
    virtual const char* getStrategyName() const = 0;
    
protected:
    void ensureGeometryCapacity(QSGGeometryNode* node, int vertexCount);
    double calculateIntensity(double liquidity, double intensityScale) const;
};