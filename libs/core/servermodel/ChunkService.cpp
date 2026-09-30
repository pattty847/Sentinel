#include "ChunkService.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <stdexcept>

namespace recording {
namespace {
Hmc2Reader& threadReader(const std::filesystem::path& root) {
    thread_local std::unique_ptr<Hmc2Reader> reader;
    thread_local std::filesystem::path readerRoot;
    if (!reader || readerRoot != root) {
        reader = std::make_unique<Hmc2Reader>(root);
        readerRoot = root;
    }
    return *reader;
}
} // namespace
std::shared_ptr<const std::vector<uint8_t>> chunkErrorFrame(const ChunkKey& key, heatmap::ChunkError code,
                                                            std::string message) {
    heatmap::ChunkFrame frame;
    frame.kind = heatmap::ChunkKind::Error;
    frame.key = key;
    // Echoed strings are length-prefixed with one byte on the wire.
    if (frame.key.symbol.size() > 255) frame.key.symbol.resize(255);
    if (frame.key.source.size() > 255) frame.key.source.resize(255);
    frame.error = code;
    if (message.size() > 255) message.resize(255);
    frame.message = std::move(message);
    return std::make_shared<const std::vector<uint8_t>>(heatmap::encodeChunk(frame));
}

ChunkService::ChunkService(std::filesystem::path root, WatermarkFn watermarks, ClockFn now, size_t cacheBytes)
    : root_(std::move(root)), watermarks_(std::move(watermarks)), now_(std::move(now)), cache_(cacheBytes) {}

BookRecorder::Watermarks ChunkService::effectiveWatermarks(const std::string& symbol,
                                                           const std::string& hmc2Layer) const {
    const auto recorded = watermarks_ ? watermarks_(symbol, hmc2Layer) : BookRecorder::Watermarks{};
    if (recorded.minuteThroughMs > 0) return recorded;
    // Margin: a series the recorder just started publishes its first watermark
    // about a minute (plus lateness) after its snapshot; until then it may still
    // write that minute and the previous hour's rollup.
    constexpr int64_t kColdMarginMs = 5 * heatmap::kMinuteMs;
    const int64_t now = now_() - kColdMarginMs;
    return {floorDiv(now, heatmap::kMinuteMs) * heatmap::kMinuteMs,
            floorDiv(now, heatmap::kHourMs) * heatmap::kHourMs - heatmap::kHourMs};
}

std::shared_ptr<const std::vector<uint8_t>> ChunkService::serve(const ChunkKey& key, uint64_t haveHash) {
    try {
        try { (void)chunkEndMs(key); }
        catch (const std::invalid_argument&) {
            return chunkErrorFrame(key, heatmap::ChunkError::InvalidRequest, "invalid chunk key");
        }
        const auto layer = hmc2Layer(key);
        auto notModified = [&](const heatmap::ChunkState& state, uint64_t hash) {
            heatmap::ChunkFrame frame;
            frame.kind = heatmap::ChunkKind::NotModified;
            frame.key = key; frame.state = state; frame.contentHash = hash;
            return std::make_shared<const std::vector<uint8_t>>(heatmap::encodeChunk(frame));
        };
        if (auto cached = cache_.get(key)) {
            if (haveHash && heatmap::chunkContentHash(*cached) == haveHash)
                return notModified({true, chunkEndMs(key), 0}, haveHash);
            return cached;
        }
        const auto watermarks = effectiveWatermarks(key.symbol, layer);
        auto state = chunkState(key, watermarks, 0);
        auto columns = buildChunk(threadReader(root_), key, watermarks);
        if (!state.sealed) {
            // Content is a function of the complete buckets before the cutoff,
            // so the covered bucket count orders open revisions.
            const int64_t scannedEnd = columns.scannedRanges.empty() ? key.startMs
                                                                     : columns.scannedRanges.back().endMs;
            state.revision = 1 + uint64_t((scannedEnd - key.startMs) / key.levelMs);
        }
        auto wire = heatmap::encodeChunk({heatmap::ChunkKind::Chunk, key, state, std::move(columns)});
        const auto hash = heatmap::chunkContentHash(wire);
        auto shared = std::make_shared<const std::vector<uint8_t>>(wire);
        if (state.sealed) cache_.put(std::move(wire));
        sLog_Probe("chunks.serve", "symbol=" << key.symbol << " source=" << key.source << " level=" << key.levelMs
                   << " start=" << key.startMs << " sealed=" << state.sealed << " rev=" << state.revision
                   << " bytes=" << shared->size() << " notModified=" << (haveHash == hash));
        if (haveHash && haveHash == hash) return notModified(state, hash);
        return shared;
    } catch (const std::exception& e) {
        sLog_Error("Chunk build failed: symbol=" << key.symbol << " source=" << key.source
                   << " level=" << key.levelMs << " start=" << key.startMs << " error=" << e.what());
        try { return chunkErrorFrame(key, heatmap::ChunkError::BuildFailed, e.what()); }
        catch (...) { return chunkErrorFrame({}, heatmap::ChunkError::BuildFailed, "chunk build failed"); }
    }
}

std::vector<ChunkService::SourceAvailability> ChunkService::availability(const std::string& symbol) {
    std::vector<SourceAvailability> out;
    auto& reader = threadReader(root_);
    for (const auto& source : heatmap::kChunkSources) {
        const std::string layer(source.hmc2Layer);
        const auto watermarks = effectiveWatermarks(symbol, layer);
        SourceAvailability item{std::string(source.id), {}, {}};
        for (const int64_t level : {heatmap::kMinuteMs, heatmap::kHourMs}) {
            const auto span = heatmap::chunkSpanMs(source.id, level);
            if (!span) continue;
            ReadControl control;
            const auto series = reader.availability(symbol, layer, level, control);
            if (control.status != ReadStatus::Complete)
                throw std::runtime_error("recording availability scan incomplete");
            LevelAvailability l{level, span,
                                level == heatmap::kMinuteMs ? watermarks.minuteThroughMs : watermarks.hourThroughMs,
                                series.oldestMs, series.latestMs};
            if (level == heatmap::kMinuteMs) item.latestGrid = series.latestHeader;
            if (l.oldestMs) item.levels.push_back(l);
        }
        if (!item.levels.empty()) out.push_back(std::move(item));
    }
    return out;
}

std::vector<int64_t> ChunkService::availabilityFingerprint(const std::string& symbol) const {
    std::vector<int64_t> out;
    for (const auto& source : heatmap::kChunkSources) {
        const auto w = effectiveWatermarks(symbol, std::string(source.hmc2Layer));
        out.push_back(w.minuteThroughMs);
        out.push_back(w.hourThroughMs);
    }
    return out;
}
} // namespace recording
