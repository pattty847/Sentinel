#pragma once
#include <QString>

namespace lab {
// Slice B1 headless benchmark (docs/research/2026-09-b1-whole-chunk-benchmark.md).
// Real recorded data, a real scene graph (OffscreenQuick) paced at 60 Hz, fresh
// lab items per session. For every prep mode (viewport V, whole-chunk W on the
// GPU, whole-chunk W on the CPU), DPR 1x/2x (physical 1500x880 / 3000x1760),
// near/deep, timeframes 1m/5m/1h and fixed preset ticks ($1/$10/$100 where the
// data supports them) it runs one scripted session:
//   cold and warm time to the first fully drawn frame; 20 small vertical pans;
//   one large vertical pan (3 views); 20 small horizontal pans; one large
//   horizontal pan (3 views back, into unprepared chunks); a price zoom-out in
//   Auto across a tick change; a revised newest chunk;
// then multi-chart memory with 1, 2 and 4 items sharing the chunk store.
// Prints a comparison table to stdout and writes all numbers to `jsonPath`.
int runB1Bench(const QString &jsonPath, bool quick);
} // namespace lab
