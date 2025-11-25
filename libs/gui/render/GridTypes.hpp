#pragma once
#include "../../core/marketdata/model/TradeData.h"
#include "../CoordinateSystem.h"
#include <QColor>
#include <QRectF>
#include <cstdint>
#include <vector>

// Shared grid rendering types to avoid circular dependencies
// World-space cell; screen-space is derived in the renderer per-frame
struct CellInstance {
    // World coordinates
    int64_t timeStart_ms = 0;
    int64_t timeEnd_ms = 0;
    double priceMin = 0.0;
    double priceMax = 0.0;

    // Display/data attributes
  float liquidity = 0.0f; // aggregated volume/liquidity
  bool isBid = true;      // side
};