#pragma once
#include <QString>

namespace lab {
// Slice S5c headless benchmark of the production heatmap path (LabData ->
// HeatmapSourceController -> HeatmapTileNode) against the B1 hybrid numbers
// (docs/research/2026-09-b1-whole-chunk-benchmark.md, results in
// docs/research/2026-09-s5-plan.md "S5c results"). Real recorded data (pin the
// range with --end-utc), a real scene graph (OffscreenQuick) paced at 60 Hz,
// fresh charts per session. For DPR 1x/2x (physical 1500x880 / 3000x1760),
// timeframes 1m/5m/1h and fixed Manual ticks $1/$10/$100 it runs one session:
//   cold and warm time to the first fully drawn frame; 20 small vertical pans;
//   one large vertical pan (3 views); 20 small horizontal pans; one large
//   horizontal pan (3 views back); a price zoom-out in Auto across a tick
//   change; a revised newest chunk; a switch to another timeframe and back
//   (the recent-tf tier);
// then multi-chart memory and decodes with 1, 2 and 4 charts sharing the path.
// Prints a table to stdout and writes all numbers to `jsonPath`.
// quick: 1x only, 1m and 1h.
int runS5Bench(const QString &jsonPath, bool quick);
} // namespace lab
