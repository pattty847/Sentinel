#include "RecordingChunks.hpp"
#include "../heatmap/RecordingLoader.hpp"
#include <algorithm>
#include <stdexcept>

namespace recording {
int64_t chunkEndMs(const ChunkKey& key) {
    const int64_t span = key.levelMs == heatmap::kMinuteMs ? heatmap::kHourMs :
                         key.levelMs == heatmap::kHourMs && key.layer == "deep" ? heatmap::kDayMs : 0;
    if (!span || key.symbol.empty() || (key.layer != "near" && key.layer != "deep") ||
        key.startMs < kHmc2MinMs || key.startMs % span || key.startMs > kHmc2EndMs - span)
        throw std::invalid_argument("invalid recording chunk key");
    return key.startMs + span;
}
ChunkState chunkState(const ChunkKey& key, int64_t committedThroughMs,
                      uint64_t revision, int64_t latenessMs) {
    const auto end = chunkEndMs(key);
    if (latenessMs < 0 || latenessMs > kHmc2EndMs - end)
        throw std::invalid_argument("invalid chunk lateness");
    const bool sealed = committedThroughMs >= end + latenessMs;
    return {sealed, committedThroughMs, sealed ? 0 : revision};
}
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key,
                                  std::optional<int64_t> committedThroughMs) {
    const auto end = chunkEndMs(key);
    const auto scanEnd = committedThroughMs ?
        std::clamp(floorDiv(*committedThroughMs, key.levelMs) * key.levelMs, key.startMs, end) : end;
    heatmap::SparseColumns out{key.symbol, key.layer, key.levelMs, key.startMs, end};
    ReadControl control;
    const auto scan = reader.visit(key.symbol, key.layer, key.levelMs, key.startMs, scanEnd,
        [&](const Hmc2Record& record) {
            if (record.observedMs) out.columns.push_back(heatmap::fromRecording(record));
        }, control);
    // A partial scan is never represented as a complete chunk. Retain the
    // reader's exact proven prefix even on future bounded-read call sites.
    if (scan.scannedEndMs > scan.scannedStartMs)
        out.scannedRanges.push_back({scan.scannedStartMs, scan.scannedEndMs});
    if (scan.status != ReadStatus::Complete || scan.scannedStartMs != key.startMs ||
        scan.scannedEndMs != scanEnd)
        throw std::runtime_error("recording chunk scan incomplete");
    heatmap::validate(out);
    return out;
}
size_t EncodedChunkLru::Hash::operator()(const ChunkKey& key) const {
    size_t h = std::hash<std::string>{}(key.symbol);
    auto mix = [&](size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); };
    mix(std::hash<std::string>{}(key.layer));
    mix(std::hash<int64_t>{}(key.levelMs));
    mix(std::hash<int64_t>{}(key.startMs));
    return h;
}
void EncodedChunkLru::put(const heatmap::ChunkFrame& frame, std::vector<uint8_t> encoded) {
    if (!frame.state.sealed || frame.kind != heatmap::ChunkKind::Chunk ||
        frame.columns.symbol != frame.key.symbol || frame.columns.layer != frame.key.layer ||
        frame.columns.tfMs != frame.key.levelMs || frame.columns.startMs != frame.key.startMs ||
        frame.columns.endMs != chunkEndMs(frame.key))
        throw std::invalid_argument("only sealed recording chunks can be cached");
    if (auto it = entries_.find(frame.key); it != entries_.end()) {
        bytes_ -= it->second->wire.size();
        lru_.erase(it->second);
        entries_.erase(it);
    }
    if (encoded.size() > maxBytes_) return;
    bytes_ += encoded.size();
    lru_.push_front({frame.key, std::move(encoded)});
    entries_[frame.key] = lru_.begin();
    while (bytes_ > maxBytes_) {
        bytes_ -= lru_.back().wire.size();
        entries_.erase(lru_.back().key);
        lru_.pop_back();
    }
}
std::optional<std::vector<uint8_t>> EncodedChunkLru::get(const ChunkKey& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return {};
    lru_.splice(lru_.begin(), lru_, it->second);
    return lru_.front().wire;
}
} // namespace recording
