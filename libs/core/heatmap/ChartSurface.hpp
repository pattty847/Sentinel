#pragma once

namespace heatmap {
// The widest chart surface supported, in device px. Time zoom-out stops at one column
// per device px on at most this many columns (maxTimeSpanMs), so a view never holds
// more bars than kMaxChartSurfaceBars: those columns plus two partially visible edges.
// Flow retention, backfill and the footprint snapshot are sized from it.
inline constexpr int kMaxChartSurfaceDevicePx = 8192;
inline constexpr int kMaxChartSurfaceBars = kMaxChartSurfaceDevicePx + 2;
} // namespace heatmap
