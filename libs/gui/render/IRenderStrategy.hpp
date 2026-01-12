/*
Sentinel — IRenderStrategy
Role: Defines the abstract interface for all rendering strategies.
Inputs/Outputs: Defines the contract for turning data from an IDataAccessor into a renderable QSGNode.
Threading: Methods are designed to be called on the Qt Quick render thread.
Performance: Interface is designed for batch operations to optimize rendering performance.
Integration: Implemented by concrete strategies (e.g., HeatmapStrategy) and used by UnifiedGridRenderer.
Observability: No diagnostics defined; responsibility of the concrete implementation.
Related: UnifiedGridRenderer.h, GridTypes.hpp, HeatmapStrategy.hpp, TradeFlowStrategy.hpp.
Assumptions: Implementations will be managed and invoked by UnifiedGridRenderer.
*/
#pragma once
#include <QSGNode>
#include "IDataAccessor.hpp"

class QSGGeometryNode;

// Forward declarations
struct CellInstance;

class IRenderStrategy {
public:
    virtual ~IRenderStrategy() = default;
    
    // Build a new node from scratch (fallback when updateNode returns false)
    virtual QSGNode* buildNode(const IDataAccessor* dataAccessor) = 0;
    
    // Update existing node in-place, returns true if successful, false if rebuild needed
    virtual bool updateNode(QSGNode* existingNode, const IDataAccessor* dataAccessor) { 
        (void)existingNode; (void)dataAccessor; 
        return false; // Default: always rebuild
    }
    
    virtual QColor calculateColor(double liquidity, bool isBid, double intensity) const = 0;
    virtual const char* getStrategyName() const = 0;
    
protected:
    void ensureGeometryCapacity(QSGGeometryNode* node, int vertexCount);
    double calculateIntensity(double liquidity, double intensityScale) const;
};