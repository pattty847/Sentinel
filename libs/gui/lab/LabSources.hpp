#pragma once
// Lab data paths: HMC2 (read-only) or synthetic -> heatmap::SparseColumns composed
// at the selected timeframe on the CPU (TimeComposer) -> heatmap::gpu::GpuSource.
#include "render/heatmap/HeatmapGpuSource.hpp"
#include <QString>
#include <memory>
#include <string>

namespace lab {
// Recording root (the directory holding BTC-USD/): SENTINEL_RECORDING_ROOT, else
// where the server records (config/server_config.yaml recording.dir, or its
// fallback_dir while dir's volume is unmounted; read relative to the working
// directory, as the server does). Empty when neither is available; real-data
// paths then report or skip.
std::string recordingRoot();
// Lab outputs must never land inside the recording root (read-only for the
// lab). False, with `why`, when `path` is inside it however it gets there
// (aliases, symlinks, junctions, not-yet-created descendants), when the path
// has a ".." component, or when either path cannot be resolved (fail closed).
// Call before creating directories and again right before writing.
bool labOutputAllowed(const QString &path, QString *why = nullptr);

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
// An empty `root` means recordingRoot(); throws when that is unset or missing.
LabSource loadRealSource(const std::string &layer, int hours, int loadHours, int64_t tfMs,
                         const std::string &root = {});

// One UTC day of synthetic minutes (about `entries` entries in total) with a grid
// change, a size-scale change, partial coverage, recorder gaps and one unloaded
// hour, composed at tfMs.
LabSource syntheticSource(uint64_t entries, int64_t tfMs);
} // namespace lab
