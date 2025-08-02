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