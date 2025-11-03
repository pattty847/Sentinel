#pragma once
#include <QRectF>
#include <QColor>
#include <vector>
#include <cstdint>
#include "../CoordinateSystem.h"

// Shared grid rendering types to avoid circular dependencies
// World-space cell; screen-space is derived in the renderer per-frame
struct CellInstance {
    // World coordinates
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;

    // Display/data attributes
    double liquidity = 0.0;   // aggregated volume/liquidity
    bool isBid = true;        // side
    double intensity = 0.0;   // normalized [0,1]
    QColor color;             // per-cell color

    // Optional: for future text overlays/annotation
    int snapshotCount = 0;
};

struct GridSliceBatch {
    std::vector<CellInstance> cells;
    double intensityScale = 1.0;
    double minVolumeFilter = 0.0;
    int maxCells = 100000;
    Viewport viewport;  // viewport snapshot for world→screen conversion
};