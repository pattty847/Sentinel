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
namespace {
int64_t throughFor(const ChunkKey& key, const BookRecorder::Watermarks& watermarks) {
    (void)chunkEndMs(key);
    return key.levelMs == heatmap::kMinuteMs ? watermarks.minuteThroughMs : watermarks.hourThroughMs;
}
heatmap::SparseColumns buildUntil(Hmc2Reader& reader, const ChunkKey& key, int64_t committedThroughMs) {
    const auto end = chunkEndMs(key);
    const auto scanEnd = committedThroughMs <= key.startMs ? key.startMs :
                         committedThroughMs >= end ? end :
                         floorDiv(committedThroughMs, key.levelMs) * key.levelMs;
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
} // namespace
ChunkState chunkState(const ChunkKey& key, const BookRecorder::Watermarks& watermarks,
                      uint64_t revision) {
    const auto end = chunkEndMs(key);
    const auto through = throughFor(key, watermarks);
    const bool sealed = through >= end;
    return {sealed, through, sealed ? 0 : revision};
}
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key) {
    return buildUntil(reader, key, chunkEndMs(key));
}
heatmap::SparseColumns buildChunk(Hmc2Reader& reader, const ChunkKey& key,
                                  const BookRecorder::Watermarks& watermarks) {
    return buildUntil(reader, key, throughFor(key, watermarks));
}
size_t EncodedChunkLru::Hash::operator()(const ChunkKey& key) const {
    size_t h = std::hash<std::string>{}(key.symbol);
    auto mix = [&](size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); };
    mix(std::hash<std::string>{}(key.layer));
    mix(std::hash<int64_t>{}(key.levelMs));
    mix(std::hash<int64_t>{}(key.startMs));
    return h;
}
void EncodedChunkLru::put(std::vector<uint8_t> encoded) {
    const auto frame = heatmap::decodeChunk(encoded);
    if (!frame.state.sealed)
        throw std::invalid_argument("only sealed recording chunks can be cached");
    std::lock_guard lock(mutex_);
    if (encoded.size() > maxBytes_) return;
    if (auto it = entries_.find(frame.key); it != entries_.end()) {
        bytes_ -= it->second->wire->size();
        lru_.erase(it->second);
        entries_.erase(it);
    }
    bytes_ += encoded.size();
    lru_.push_front({frame.key, std::make_shared<const std::vector<uint8_t>>(std::move(encoded))});
    entries_[frame.key] = lru_.begin();
    while (bytes_ > maxBytes_) {
        bytes_ -= lru_.back().wire->size();
        entries_.erase(lru_.back().key);
        lru_.pop_back();
    }
}
std::shared_ptr<const std::vector<uint8_t>> EncodedChunkLru::get(const ChunkKey& key) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) return {};
    lru_.splice(lru_.begin(), lru_, it->second);
    return lru_.front().wire;
}
size_t EncodedChunkLru::bytes() const { std::lock_guard lock(mutex_); return bytes_; }
size_t EncodedChunkLru::size() const { std::lock_guard lock(mutex_); return entries_.size(); }
} // namespace recording
