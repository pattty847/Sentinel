#pragma once

namespace heatmap {
// The thinnest chart cell, in logical px (owner 2026-11-10): time zoom-out stops at one
// column per this width, a Manual tick at one row per this height (maxTimeSpanMs,
// maxManualPriceSpan). The span limit is the only zoom-out limit; cameras add no floor.
inline constexpr double kMinChartCellLogicalPx = 2;
// The most columns supported in one view: full zoom-out on a surface up to
// kMaxChartColumns * kMinChartCellLogicalPx (16384) logical px wide.
inline constexpr int kMaxChartColumns = 8192;
// Every bar of such a view: those columns plus two partially visible edges. Flow
// retention and backfill, the footprint snapshot and the CVD series are sized from it.
// A wider surface keeps the newest bars and logs the trimmed far past.
inline constexpr int kMaxChartSurfaceBars = kMaxChartColumns + 2;
} // namespace heatmap
