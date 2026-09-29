#pragma once
#include <QMetaType>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <cstdint>

// Immutable data-thread -> render-thread grid. No heatmap state participates.
struct TradeOverlayGrid {
    int64_t startMs = 0, endMs = 0;
    double maxPrice = 0, tick = 0;
    uint64_t generation = 0;
};
Q_DECLARE_METATYPE(TradeOverlayGrid)
struct TradeOverlayMapping {
    QRectF draw, source;
};
inline TradeOverlayMapping mapTradeOverlay(const TradeOverlayGrid& grid, int width, int rows,
    int64_t viewStart, int64_t viewEnd, double viewMin, double viewMax, const QRectF& surface) {
    if (width <= 0 || rows <= 0 || grid.endMs <= grid.startMs || viewEnd <= viewStart ||
        !std::isfinite(grid.maxPrice) || !std::isfinite(grid.tick) || grid.tick <= 0 ||
        !std::isfinite(viewMin) || !std::isfinite(viewMax) || viewMax <= viewMin || surface.isEmpty()) return {};
    const auto start = std::max(viewStart, grid.startMs), end = std::min(viewEnd, grid.endMs);
    const double high = std::min(viewMax, grid.maxPrice);
    const double low = std::max(viewMin, grid.maxPrice - rows * grid.tick);
    if (end <= start || high <= low) return {};
    const double x = surface.left() + double(start - viewStart) / (viewEnd - viewStart) * surface.width();
    const double y = surface.top() + (viewMax - high) / (viewMax - viewMin) * surface.height();
    return {{x, y, double(end - start) / (viewEnd - viewStart) * surface.width(),
                  (high - low) / (viewMax - viewMin) * surface.height()},
            {double(start - grid.startMs) / (grid.endMs - grid.startMs) * width,
             (grid.maxPrice - high) / grid.tick,
             double(end - start) / (grid.endMs - grid.startMs) * width, (high - low) / grid.tick}};
}
