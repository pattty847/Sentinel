#pragma once
#include <QSGNode>
#include <QColor>
#include <vector>
#include "../LiquidityTimeSeriesEngine.h"

class QSGGeometryNode;

struct CellInstance {
    QRectF screenRect;
    QColor color;
    double intensity;
    double liquidity;
    bool isBid;
    int64_t timeSlot;
    double priceLevel;
};

struct GridSliceBatch {
    std::vector<CellInstance> cells;
    double intensityScale = 1.0;
    double minVolumeFilter = 0.0;
    int maxCells = 100000;
};

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