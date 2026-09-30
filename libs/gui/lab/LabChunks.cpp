#include "LabChunks.hpp"
#include "LabSources.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/RecordingChunks.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
using heatmap::kHourMs;
using heatmap::kMinuteMs;

std::mutex rootMutex;
std::string recordingRoot = kRecordingRoot;
std::string currentRoot() {
    std::scoped_lock lock(rootMutex);
    return recordingRoot;
}

std::mutex infoMutex;
std::map<std::string, LayerInfo> infos;

LayerInfo readLayerInfo(const std::string &layer) {
    LayerInfo info;
    try {
        recording::Hmc2Reader reader(currentRoot()); // read-only; never takes the writer lock
        recording::ReadControl control;
        const auto minutes = reader.availability(kSymbol, layer, kMinuteMs, control);
        const auto hours = layer == "deep" ? reader.availability(kSymbol, layer, kHourMs, control)
                                           : recording::SeriesAvailability{};
        if (control.status != recording::ReadStatus::Complete || !minutes.latestMs) {
            info.error = "no " + layer + " minute recording available";
            return info;
        }
        auto &a = info.availability;
        a.endMs = *minutes.latestMs + kMinuteMs;
        a.oldestMs = minutes.oldestMs.value_or(a.endMs);
        if (hours.oldestMs) a.oldestMs = std::min(a.oldestMs, *hours.oldestMs);
        a.hourThroughMs = hours.latestMs ? *hours.latestMs + kHourMs : 0;
    } catch (const std::exception &e) {
        info.error = e.what();
    }
    return info;
}

heatmap::ChunkStore::Loaded loadChunk(const heatmap::ChunkKey &key) {
    // Hmc2Reader is thread-affine: one per worker thread, reused across chunks.
    thread_local std::unique_ptr<recording::Hmc2Reader> reader;
    thread_local std::string readerRoot;
    const std::string root = currentRoot();
    if (!reader || readerRoot != root) {
        reader = std::make_unique<recording::Hmc2Reader>(root);
        readerRoot = root;
    }
    const auto info = layerInfo(key.layer);
    if (!info.error.empty()) throw std::runtime_error(info.error);
    const recording::BookRecorder::Watermarks watermarks{info.availability.endMs, info.availability.hourThroughMs};
    heatmap::ChunkStore::Loaded loaded;
    loaded.columns = recording::buildChunk(*reader, key, watermarks);
    loaded.sealed = recording::chunkState(key, watermarks, 0).sealed;
    return loaded;
}

// ------------------------------------------------------------------ W intermediates
struct InterKey {
    std::string layer;
    int64_t tfMs = 0, tile = 0;
    uint64_t generation = 0;
    TileBuilder builder = TileBuilder::Gpu;
    bool operator==(const InterKey &) const = default;
};
struct InterKeyHash {
    size_t operator()(const InterKey &k) const {
        size_t h = std::hash<std::string>{}(k.layer);
        auto mix = [&](size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); };
        mix(std::hash<int64_t>{}(k.tfMs));
        mix(std::hash<int64_t>{}(k.tile));
        mix(std::hash<uint64_t>{}(k.generation));
        mix(size_t(k.builder));
        return h;
    }
};
struct Intermediate {
    std::shared_ptr<const heatmap::SparseColumns> composed;  // Cpu
    std::shared_ptr<const heatmap::gpu::GpuSource> gpu;     // Gpu
    heatmap::tiles::UnitExtent units;
    double priceScale = 100;
};
std::mutex interMutex;
heatmap::tiles::ByteLru<InterKey, Intermediate, InterKeyHash> intermediates(256ull << 20);

double priceScaleOf(const heatmap::SparseColumns &columns) {
    for (const auto &c : columns.columns)
        for (const auto &n : c.native) return n.grid.priceScale;
    return 100;
}
std::vector<std::shared_ptr<const heatmap::StoredChunk>> getChunks(const std::vector<heatmap::ChunkKey> &keys,
                                                                   PrepTiming &timing) {
    const auto started = Clock::now();
    timing.chunkLoadsBefore = chunkStore().stats().loads;
    std::vector<std::shared_ptr<const heatmap::StoredChunk>> chunks;
    chunks.reserve(keys.size());
    for (const auto &key : keys) chunks.push_back(chunkStore().get(key));
    timing.chunkLoadsAfter = chunkStore().stats().loads;
    timing.chunks = uint32_t(chunks.size());
    timing.chunkMs = msSince(started);
    return chunks;
}
std::vector<std::pair<heatmap::ChunkKey, uint64_t>> generationsOf(
    const std::vector<std::shared_ptr<const heatmap::StoredChunk>> &chunks) {
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> out;
    for (const auto &c : chunks) out.emplace_back(c->key, c->generation);
    return out;
}
} // namespace

heatmap::ChunkStore &chunkStore() {
    static heatmap::ChunkStore store(512ull << 20, loadChunk);
    return store;
}
void setChunkRecordingRoot(const std::string &root) {
    {
        std::scoped_lock lock(rootMutex);
        recordingRoot = root;
    }
    {
        std::scoped_lock lock(infoMutex);
        infos.clear();
    }
    chunkStore().clear();
    clearIntermediates();
}

LayerInfo layerInfo(const std::string &layer, bool refresh) {
    {
        std::scoped_lock lock(infoMutex);
        const auto it = infos.find(layer);
        if (it != infos.end() && !refresh) return it->second;
    }
    auto info = readLayerInfo(layer);
    std::scoped_lock lock(infoMutex);
    infos[layer] = info;
    return info;
}

double recentMedianPrice(const std::string &layer) {
    const auto info = layerInfo(layer);
    if (!info.error.empty()) return 0;
    const int64_t newest = recording::floorDiv(info.availability.endMs - 1, kHourMs) * kHourMs;
    for (int64_t start = newest; start >= newest - 3 * kHourMs; start -= kHourMs) {
        const auto chunk = chunkStore().get({kSymbol, layer, kMinuteMs, start});
        const double price = medianRecentPrice(*chunk->columns);
        if (price > 0) return price;
    }
    return 0;
}

ViewportSource buildViewportSource(const std::string &layer, int64_t tfMs, const heatmap::gpu::ViewWindow &region) {
    const auto started = Clock::now();
    const auto info = layerInfo(layer);
    if (!info.error.empty()) throw std::runtime_error(info.error);
    const auto &a = info.availability;
    int64_t start = recording::floorDiv(int64_t(std::floor(region.timeLoMs)), tfMs) * tfMs;
    int64_t end = recording::floorDiv(int64_t(std::ceil(region.timeHiMs)) + tfMs - 1, tfMs) * tfMs;
    start = std::max(start, recording::floorDiv(a.oldestMs, tfMs) * tfMs);
    end = std::min(end, recording::floorDiv(a.endMs + tfMs - 1, tfMs) * tfMs);
    if (end <= start) end = start + tfMs;
    ViewportSource out;
    const auto keys = heatmap::tiles::chunksFor(kSymbol, layer, tfMs, start, end, a);
    const auto chunks = getChunks(keys, out.timing);
    out.chunkGenerations = generationsOf(chunks);
    out.generation = heatmap::tiles::combineGenerations(chunks);
    const auto composedAt = Clock::now();
    const auto composed = heatmap::tiles::composeChunks(chunks, kSymbol, layer, tfMs, start, end);
    out.timing.composeMs = msSince(composedAt);
    heatmap::gpu::GpuSourceOptions options;
    options.availableStartMs = a.oldestMs;
    options.availableEndMs = a.endMs;
    options.priceLo = region.priceLo;
    options.priceHi = region.priceHi;
    const auto builtAt = Clock::now();
    out.gpu = std::make_shared<const heatmap::gpu::GpuSource>(heatmap::gpu::buildGpuSource(composed, options));
    out.timing.buildMs = msSince(builtAt);
    out.cpuBytes = out.gpu->bytes();
    out.region = {double(start), double(end), region.priceLo, region.priceHi};
    out.timing.totalMs = msSince(started);
    return out;
}

TileBuild buildTile(const std::string &layer, int64_t tfMs, int64_t tickUnits, int64_t tile, TileBuilder builder,
                    int64_t centerBin, const recording::SizeScale &outputScale) {
    const auto started = Clock::now();
    const auto info = layerInfo(layer);
    if (!info.error.empty()) throw std::runtime_error(info.error);
    const auto &a = info.availability;
    TileBuild out;
    out.key = {kSymbol, layer, tfMs, tickUnits, tile, 0};
    const int64_t start = heatmap::tiles::tileStartMs(tile, tfMs), end = heatmap::tiles::tileEndMs(tile, tfMs);
    const auto keys = heatmap::tiles::chunksFor(kSymbol, layer, tfMs, start, end, a);
    if (keys.empty()) {
        out.empty = true;
        return out;
    }
    const auto chunks = getChunks(keys, out.timing);
    out.chunkGenerations = generationsOf(chunks);
    out.key.sourceGeneration = heatmap::tiles::combineGenerations(chunks);
    const InterKey interKey{layer, tfMs, tile, out.key.sourceGeneration, builder};
    Intermediate inter;
    {
        std::scoped_lock lock(interMutex);
        if (const auto *hit = intermediates.find(interKey)) {
            inter = *hit;
            out.intermediateHit = true;
        }
    }
    if (!out.intermediateHit) {
        const auto composedAt = Clock::now();
        auto composed = std::make_shared<const heatmap::SparseColumns>(
            heatmap::tiles::composeChunks(chunks, kSymbol, layer, tfMs, start, end));
        out.timing.composeMs = msSince(composedAt);
        inter.priceScale = priceScaleOf(*composed);
        inter.units = heatmap::tiles::usefulUnits(*composed, inter.priceScale);
        size_t bytes = 0;
        if (builder == TileBuilder::Gpu) {
            const auto builtAt = Clock::now();
            heatmap::gpu::GpuSourceOptions options;
            options.availableStartMs = a.oldestMs;
            options.availableEndMs = a.endMs;
            inter.gpu = std::make_shared<const heatmap::gpu::GpuSource>(heatmap::gpu::buildGpuSource(*composed, options));
            inter.priceScale = inter.gpu->priceScale;
            out.timing.buildMs += msSince(builtAt);
            bytes = inter.gpu->bytes();
        } else {
            inter.composed = composed;
            bytes = heatmap::sparseBytes(*composed);
        }
        std::scoped_lock lock(interMutex);
        intermediates.insert(interKey, inter, bytes);
        intermediates.evict();
    }
    auto extent = heatmap::tiles::binsOf(inter.units, tickUnits);
    if (extent.empty()) extent = {centerBin, centerBin + 1}; // gaps/loading only: sentinel rows carry the state
    const auto grid = heatmap::tiles::tileGrid(tile, tfMs, tickUnits, inter.priceScale, extent, centerBin, &out.clipped);
    if (!grid) throw std::runtime_error("tile grid failed");
    out.grid = *grid;
    if (builder == TileBuilder::Gpu) {
        out.source = inter.gpu;
    } else {
        const auto builtAt = Clock::now();
        heatmap::tiles::CellOptions options;
        options.availableStartMs = a.oldestMs;
        options.availableEndMs = a.endMs;
        options.outputScale = outputScale;
        out.cells = std::make_shared<const std::vector<uint32_t>>(heatmap::tiles::buildCells(*inter.composed, *grid, options));
        out.timing.buildMs += msSince(builtAt);
    }
    out.timing.totalMs = msSince(started);
    return out;
}

std::optional<TileBuild> cachedGpuTile(const std::string &layer, int64_t tfMs, int64_t tickUnits, int64_t tile,
                                       int64_t centerBin) {
    const auto info = layerInfo(layer);
    if (!info.error.empty()) return std::nullopt;
    const int64_t start = heatmap::tiles::tileStartMs(tile, tfMs), end = heatmap::tiles::tileEndMs(tile, tfMs);
    const auto keys = heatmap::tiles::chunksFor(kSymbol, layer, tfMs, start, end, info.availability);
    if (keys.empty()) return std::nullopt;
    std::vector<std::shared_ptr<const heatmap::StoredChunk>> chunks;
    for (const auto &key : keys) {
        auto chunk = chunkStore().cached(key);
        if (!chunk) return std::nullopt;
        chunks.push_back(std::move(chunk));
    }
    TileBuild out;
    out.key = {kSymbol, layer, tfMs, tickUnits, tile, heatmap::tiles::combineGenerations(chunks)};
    Intermediate inter;
    {
        std::scoped_lock lock(interMutex);
        const auto *hit = intermediates.find({layer, tfMs, tile, out.key.sourceGeneration, TileBuilder::Gpu});
        if (!hit) return std::nullopt;
        inter = *hit;
    }
    out.chunkGenerations = generationsOf(chunks);
    out.intermediateHit = true;
    auto extent = heatmap::tiles::binsOf(inter.units, tickUnits);
    if (extent.empty()) extent = {centerBin, centerBin + 1};
    const auto grid = heatmap::tiles::tileGrid(tile, tfMs, tickUnits, inter.priceScale, extent, centerBin, &out.clipped);
    if (!grid) return std::nullopt;
    out.grid = *grid;
    out.source = inter.gpu;
    return out;
}

IntermediateStats intermediateStats() {
    std::scoped_lock lock(interMutex);
    return {intermediates.hits(), intermediates.misses(), intermediates.evictions(), intermediates.bytes(),
            intermediates.size(), intermediates.maxBytes()};
}
void clearIntermediates() {
    std::scoped_lock lock(interMutex);
    intermediates.clear();
}
void resetIntermediateStats() {
    std::scoped_lock lock(interMutex);
    intermediates.resetStats();
}

std::vector<heatmap::ChunkKey> chunkKeysFor(const std::string &layer, int64_t tfMs, int64_t startMs, int64_t endMs) {
    const auto info = layerInfo(layer);
    if (!info.error.empty()) return {};
    return heatmap::tiles::chunksFor(kSymbol, layer, tfMs, startMs, endMs, info.availability);
}

namespace {
std::vector<std::shared_ptr<const heatmap::StoredChunk>> cachedChunks(const std::string &layer, int64_t tfMs,
                                                                     double loMs, double hiMs) {
    const int64_t start = int64_t(std::floor(loMs / double(tfMs))) * tfMs;
    const int64_t end = int64_t(std::ceil(hiMs / double(tfMs))) * tfMs;
    std::vector<std::shared_ptr<const heatmap::StoredChunk>> out;
    for (const auto &key : chunkKeysFor(layer, tfMs, start, end))
        if (auto chunk = chunkStore().cached(key)) out.push_back(std::move(chunk));
    return out;
}
} // namespace

int64_t cachedCommonUnits(const std::string &layer, int64_t tfMs, double loMs, double hiMs) {
    const auto chunks = cachedChunks(layer, tfMs, loMs, hiMs);
    return heatmap::tiles::commonUnitsIn(chunks, loMs, hiMs, tfMs);
}

std::vector<int64_t> cachedColumnCommonUnits(const std::string &layer, int64_t tfMs, double loMs, double hiMs) {
    std::set<int64_t> out;
    for (const auto &chunk : cachedChunks(layer, tfMs, loMs, hiMs))
        for (const auto &column : chunk->columns->columns) {
            int64_t lcm = 0;
            for (const auto &n : column.native)
                if (n.grid.rowTickUnits > 0) lcm = lcm ? std::lcm(lcm, n.grid.rowTickUnits) : n.grid.rowTickUnits;
            if (lcm) out.insert(lcm);
        }
    return {out.begin(), out.end()};
}

uint64_t processFootprintBytes() {
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.phys_footprint;
#endif
    return 0;
}
} // namespace lab
