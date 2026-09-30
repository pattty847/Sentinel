#pragma once
// Slice B1 data layer for the lab: one process-wide decoded-chunk cache over the
// HMC2 recording (read-only), shared by every lab item, and the two ways of
// preparing a heatmap from it that B1 compares:
//   V (viewport): compose the chunks of the view's time range plus one view width
//     each side, clip rows to the view's price range plus one view height each
//     side, build one GpuSource; HeatmapRenderNode bins a screen-sized grid.
//   W (whole chunk): compose one 64-column tile span, bin its WHOLE useful price
//     extent at the tick into render-ready cells, on the GPU (the tile's
//     GpuSource is binned into the tile's buffer) or on the CPU (binColumn).
// Tick-independent intermediates of W (a tile span's composed columns or its
// GpuSource, plus its useful price extent) are cached process-wide, so a tick
// change rebuilds cells without recomposing.
// All functions are thread-safe; builders run on worker threads.
#include "heatmap/ChunkStore.hpp"
#include <QObject>
#include "heatmap/HeatmapTiles.hpp"
#include "render/heatmap/HeatmapBinGrid.hpp"
#include "render/heatmap/HeatmapGpuSource.hpp"
#include <memory>
#include <optional>
#include <string>

namespace lab {
inline constexpr const char *kSymbol = "BTC-USD";

// Process-wide caches (plan budgets: 512 MiB decoded chunks; tile intermediates 256 MiB).
heatmap::ChunkStore &chunkStore();
void setChunkRecordingRoot(const std::string &root); // tests; empty restores recordingRoot()
// Pins the recording's end (exclusive, UTC ms; 0 = live): availability, and so
// every chunk, stops there. Benchmarks use a closed past range so every mode
// sees the same sealed data. Clears the caches.
void setPinnedEndMs(int64_t endMs);
int64_t pinnedEndMs();

// Queued (GUI-thread) notification of every chunk revision in the store, so
// every chart re-checks its prepared data, not only the one that asked.
class ChunkEvents : public QObject {
    Q_OBJECT
signals:
    void chunkRevised();
};
ChunkEvents *chunkEvents(); // lives on the thread of the QCoreApplication

struct LayerInfo {
    heatmap::tiles::Availability availability;
    std::string error; // non-empty: unavailable
};
// Cached per layer; refresh re-reads availability from the recording.
LayerInfo layerInfo(const std::string &layer, bool refresh = false);

// Median best-bid/best-ask midpoint of the newest recorded columns (initial view).
double recentMedianPrice(const std::string &layer);

struct PrepTiming {
    double chunkMs = 0, composeMs = 0, buildMs = 0, totalMs = 0;
    uint32_t chunks = 0;          // native chunks used
    uint64_t chunkLoadsBefore = 0, chunkLoadsAfter = 0; // store decode counter around the build
};

// ------------------------------------------------------------------ V
struct ViewportSource {
    std::shared_ptr<const heatmap::gpu::GpuSource> gpu;
    heatmap::gpu::ViewWindow region; // prepared time range (tf aligned, clipped to data) and row clip
    uint64_t generation = 0;         // combineGenerations of its chunks
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> chunkGenerations;
    PrepTiming timing;
    size_t cpuBytes = 0;             // the GpuSource's CPU image
};
// region: the wanted time/price window (already widened by the caller's margin).
ViewportSource buildViewportSource(const std::string &layer, int64_t tfMs, const heatmap::gpu::ViewWindow &region);

// ------------------------------------------------------------------ W
enum class TileBuilder { Gpu, Cpu };
struct TileBuild {
    heatmap::tiles::TileKey key;
    heatmap::tiles::TileGrid grid;
    std::shared_ptr<const std::vector<uint32_t>> cells;           // Cpu builder
    std::shared_ptr<const heatmap::gpu::GpuSource> source;        // Gpu builder
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> chunkGenerations;
    PrepTiming timing;
    bool intermediateHit = false; // composed span / GpuSource came from the cache
    bool clipped = false;         // extent exceeded kMaxTileRows
    bool empty = false;           // nothing available in this span
};
TileBuild buildTile(const std::string &layer, int64_t tfMs, int64_t tickUnits, int64_t tile, TileBuilder builder,
                    int64_t centerBin, const recording::SizeScale &outputScale = {});

// The same tile without any decode, compose or build: only when every chunk and
// the tile span's GpuSource are cached (Gpu builder only). Lets a tick change of
// already prepared spans reach the renderer in the same frame.
std::optional<TileBuild> cachedGpuTile(const std::string &layer, int64_t tfMs, int64_t tickUnits, int64_t tile,
                                       int64_t centerBin);

struct IntermediateStats { uint64_t hits = 0, misses = 0, evictions = 0; size_t bytes = 0, entries = 0, maxBytes = 0; };
IntermediateStats intermediateStats();
void clearIntermediates();
void resetIntermediateStats();

// LCM of native tick units of the stored chunks' columns in [loMs, hiMs) at tfMs
// (0 when none are cached). Never loads.
int64_t cachedCommonUnits(const std::string &layer, int64_t tfMs, double loMs, double hiMs);
// Distinct per-column common tick units of the cached chunks in range (Manual presets).
std::vector<int64_t> cachedColumnCommonUnits(const std::string &layer, int64_t tfMs, double loMs, double hiMs);
// The chunk keys of a time range (availability applied) and which of them are cached.
std::vector<heatmap::ChunkKey> chunkKeysFor(const std::string &layer, int64_t tfMs, int64_t startMs, int64_t endMs);

// Process physical footprint (macOS task_vm_info; includes GPU memory on unified memory), bytes.
uint64_t processFootprintBytes();
} // namespace lab
