#pragma once
#include <QString>
#include <cstdint>

namespace lab {
class LabItem;
// Command-line state shared by the interactive lab, --screenshot and --tick-sweep
// so the orchestrator can reproduce experiment states (E1-E3) headlessly.
struct LabRunOptions {
    int hours = 24;
    QString layer = QStringLiteral("near");
    uint32_t synthetic = 0;
    int tfMinutes = 1;
    bool manual = false;       // --tick-mode manual
    double tick = 0;           // --tick: Manual preset (implies manual)
    double hysteresis = 0.25;  // --hysteresis
    bool hysteresisSet = false;
    double minRowPx = 2;       // --min-row-px
    double zoomRowsPx = 0;     // --zoom-rows-px: one commonTick() row this many physical px tall
    bool crossfade = true;     // 150 ms (spec rule 8); --no-crossfade for a hard switch
    double panColumns = 0;     // --pan-columns (screenshot only)
    QString prep = QStringLiteral("full"); // --prep: full | viewport | whole-chunk | whole-chunk-cpu (B1)
    int charts = 1;            // --charts: lab items sharing one process-wide chunk store (B1)
};
// Applies the tick options to an item (before or after its source loads).
void applyTickOptions(LabItem &item, const LabRunOptions &options);
// Headless Metal benchmark of HeatmapGpuBinner compute passes (1x and 2x grids).
int runBench(int hours, const QString &layer, uint32_t synthetic, int tfMinutes = 1);
// Renders the lab item (HeatmapRenderNode in a real scene graph) offscreen once
// its full source is uploaded and drawn, stamps the debug state (mode, tick, h,
// commonTick, re-bin CPU submit time, resolution indicator) on the image, and writes a PNG.
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
