#pragma once
#include <QString>
#include <cstdint>

namespace lab {
class LabItem;
// Command-line state shared by the interactive lab, --screenshot and --tick-sweep
// so the orchestrator can reproduce experiment states (E1-E4) headlessly.
struct LabRunOptions {
    int hours = 24;
    QString layer = QStringLiteral("near"); // --bench only (one HMC2 layer as one source)
    uint32_t synthetic = 0;                 // --bench only
    int tfMinutes = 1;
    bool manual = false;       // --tick-mode manual
    double tick = 0;           // --tick: Manual preset (implies manual)
    double hysteresis = 0.25;  // --hysteresis
    bool hysteresisSet = false;
    double minRowPx = 2;       // --min-row-px
    double zoomRowsPx = 0;     // --zoom-rows-px: one commonTick() row this many physical px tall
    bool crossfade = true;     // 150 ms (spec rule 8); --no-crossfade for a hard switch
    double panColumns = 0;     // --pan-columns (screenshot only)
    int charts = 1;            // --charts: lab items sharing one data path (fetcher, store, span cache)
    bool bandEdges = false;    // --band-edges: E4, draw the finest source's band edges
    double centerPrice = 0;    // --center-price: screenshot view centred here (0 = recent mid)
    double priceSpan = 0;      // --price-span: screenshot view height in price units (0 = default)
    uint64_t gpuCapBytes = 0;  // --gpu-cap-mb (0 = HeatmapBudgets default)
};
// Applies the tick options to an item (before or after its source loads).
void applyTickOptions(LabItem &item, const LabRunOptions &options);
// Headless benchmark of HeatmapGpuBinner compute passes (1x and 2x grids) on the
// selected QRhi backend (RhiBackend.hpp).
int runBench(int hours, const QString &layer, uint32_t synthetic, int tfMinutes = 1);
// Renders the lab item (HeatmapTileNode in a real scene graph) offscreen once
// settled, stamps the debug state (mode, tick, h, row height, last re-bin, resident
// MB, refused spans, resolution indicator; with --band-edges the near band edges)
// on the image, and writes a PNG.
int runScreenshot(const LabRunOptions &options, const QString &path);
// Experiment E1 headless: zooms the price axis in and out through a range of row
// heights (and jitters one step around every change point) for each h in
// {0, 0.15, 0.25, 0.4} (or --hysteresis), logging every tick change as a JSON line
// and a summary per h (tick changes, jitter flips, re-bin CPU submit ms, GPU frame ms).
int runTickSweep(const LabRunOptions &options);
// Experiment E2 headless: once settled, zooms price out 5x (forcing an Auto tick
// change) and saves a frame roughly every 25 ms for 300 ms into `dir`
// (150 ms crossfade by default, hard switch with --no-crossfade).
int runTickChangeSequence(const LabRunOptions &options, const QString &dir);
} // namespace lab
