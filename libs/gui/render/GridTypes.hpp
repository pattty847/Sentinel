#pragma once
#include <QRectF>
#include <QColor>
#include <vector>
#include <cstdint>

// Shared grid rendering types to avoid circular dependencies
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