#pragma once
#include <QString>
#include <cstdint>

namespace lab {
// Headless Metal benchmark of HeatmapGpuBinner compute passes (1x and 2x grids).
int runBench(int hours, const QString &layer, uint32_t synthetic, int tfMinutes = 1);
// Renders the lab item (HeatmapRenderNode in a real scene graph) offscreen once
// its full source is uploaded and drawn, and writes a PNG.
int runScreenshot(int hours, const QString &layer, uint32_t synthetic, int tfMinutes, const QString &path,
                  double panColumns = 0);
} // namespace lab
