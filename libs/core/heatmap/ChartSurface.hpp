#pragma once
#include <algorithm>
#include <cmath>

namespace heatmap {
// The widest chart surface supported, in device px. Time zoom-out stops at one column
// per device px on at most this many columns (maxTimeSpanMs), so a view never holds
// more bars than kMaxChartSurfaceBars: those columns plus two partially visible edges.
// Flow retention, backfill and the footprint snapshot are sized from it.
inline constexpr int kMaxChartSurfaceDevicePx = 8192;
inline constexpr int kMaxChartSurfaceBars = kMaxChartSurfaceDevicePx + 2;
// The narrowest whole-device-px column a surface of widthPx may draw: a surface wider
// than kMaxChartSurfaceDevicePx draws wider columns so it never holds more columns.
inline int minColumnDevicePx(double widthPx) {
    if (!std::isfinite(widthPx) || !(widthPx > 0)) return 1;
    const double width = std::floor(widthPx + 1e-6); // as devicePixels() rounds the surface
    return std::max(1, int(std::ceil(width / kMaxChartSurfaceDevicePx)));
}
} // namespace heatmap
