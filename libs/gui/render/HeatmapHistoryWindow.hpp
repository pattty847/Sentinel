#pragma once

#include "../datasources/IGridDataSource.hpp"

#include <QVector>
#include <cstdint>

namespace heatmap_history {

struct Window {
    QVector<IGridDataSource::HeatmapHistoryColumn> columns;
    int intensityBytesPerCell = 0;
    int gridHeight = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
};

// Builds one dense, chronological GPU window. Missing buckets become zero
// columns and source price bands are resampled into one range so recentering
// does not reinterpret row indices from older columns.
bool buildWindow(const QVector<IGridDataSource::HeatmapHistoryColumn>& source,
                 int64_t timeframeMs,
                 int capacity,
                 int gridHeight,
                 int64_t requestedEndMs,
                 Window& out);

} // namespace heatmap_history
