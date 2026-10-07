/*
Sentinel — HeatmapWalls
Role: the GET /api/v1/heatmap/walls query and answer, scanned by HeatmapCellQuery
      over the GPU heatmap's resident picture (HeatmapGpuLayer::scanWalls).
*/
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace heatmap {

struct WallQuery {
    std::optional<int64_t> startMs, endMs;
    std::optional<double> priceMin, priceMax;
    double minQty = 0.0;
    int limit = 20;
    std::optional<double> tick; // unset uses the drawn tick
};

// One price cell and side over the scanned time range: a wall is a level, not a
// minute. qty is its peak aggregated resting size; bucketStartMs is when it peaked.
struct Wall {
    int64_t bucketStartMs = 0;
    double priceLow = 0, priceHigh = 0;
    bool ask = false;
    double qty = 0, notional = 0;
    bool forming = false;
    double meanQty = 0;          // mean over recorded columns in range (absent = 0)
    int64_t firstSeenMs = 0, lastSeenMs = 0;
    int columns = 0;             // recorded columns where the cell held this side
};

enum class WallError { None, BadTick, InvalidRange, ScanLimit };

struct WallsSnapshot {
    int status = 200;
    bool gpuRenderer = false;
    int64_t loadedStartMs = 0, loadedEndMs = 0;
    double bandTick = 0;
    int recordedColumns = 0, missingColumns = 0;
    bool unknownRows = false;
    std::vector<Wall> walls;
    int64_t rangeStartMs = 0, rangeEndMs = 0;
    double rangePriceMin = 0, rangePriceMax = 0;
    WallError error = WallError::None;
};

} // namespace heatmap
