#pragma once
// Lab data paths: HMC2 (read-only) or synthetic -> heatmap::SparseColumns composed
// at the selected timeframe on the CPU (TimeComposer) -> heatmap::gpu::GpuSource.
#include "render/heatmap/HeatmapGpuSource.hpp"
#include <memory>
#include <string>

namespace lab {
inline constexpr const char *kRecordingRoot = "/Volumes/T7/sentinel-data/recording";

struct LabSource {
    std::shared_ptr<const heatmap::SparseColumns> columns;
    std::shared_ptr<const heatmap::gpu::GpuSource> gpu;
    int64_t startMs = 0, endMs = 0;        // loaded (scanned) range
    int64_t availableStartMs = 0;          // oldest recorded time for this layer
    double loadMs = 0, composeMs = 0, buildMs = 0;
    uint64_t sparseEntries = 0;
    double medianPrice = 0;                // of recent entries, for the initial view
};

// Median over the newest 30 columns of the best-bid/best-ask midpoint (0 if none).
double medianRecentPrice(const heatmap::SparseColumns &data);

// Range [endMs - hours, endMs) ending at the newest recorded minute, aligned to
// tfMs. Deep hour multiples use persisted hours plus the open hour's minute tail
// (heatmap::loadRecordingLevels); everything else composes minutes. `loadHours`
// may be smaller than `hours` to load just the recent part first; the source
// then advertises the rest as available-but-not-loaded (drawn as "loading").
LabSource loadRealSource(const std::string &layer, int hours, int loadHours, int64_t tfMs,
                         const std::string &root = kRecordingRoot);

// One UTC day of synthetic minutes (about `entries` entries in total) with a grid
// change, a size-scale change, partial coverage, recorder gaps and one unloaded
// hour, composed at tfMs.
LabSource syntheticSource(uint64_t entries, int64_t tfMs);
} // namespace lab
