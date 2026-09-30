#include "ChunkStore.hpp"
#include <atomic>
#include <chrono>

namespace heatmap {
namespace {
std::atomic<uint64_t> nextGeneration{1};
template <class T> size_t vectorBytes(const std::vector<T> &v) { return v.capacity() * sizeof(T); }
} // namespace

size_t sparseBytes(const SparseColumns &data) {
    size_t total = sizeof(SparseColumns) + data.symbol.capacity() + data.layer.capacity() +
                   vectorBytes(data.columns) + vectorBytes(data.scannedRanges);
    for (const auto &column : data.columns) {
        total += vectorBytes(column.native);
        for (const auto &native : column.native) {
            total += vectorBytes(native.entries) + vectorBytes(native.entryCoveredMs) + vectorBytes(native.numerators);
            for (const auto &runs : native.coverage) total += vectorBytes(runs);
        }
    }
    return total;
}

size_t ChunkKeyHash::operator()(const ChunkKey &key) const {
    size_t h = std::hash<std::string>{}(key.symbol);
    auto mix = [&](size_t v) { h ^= v + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2); };
    mix(std::hash<std::string>{}(key.layer));
    mix(std::hash<int64_t>{}(key.levelMs));
    mix(std::hash<int64_t>{}(key.startMs));
    return h;
}

ChunkStore::ChunkStore(size_t maxBytes, Loader loader) : loader_(std::move(loader)), maxBytes_(maxBytes) {}

std::shared_ptr<const StoredChunk> ChunkStore::insertLocked(const ChunkKey &key, Loaded loaded) {
    auto chunk = std::make_shared<StoredChunk>();
    chunk->key = key;
    chunk->sealed = loaded.sealed;
    chunk->revision = loaded.revision;
    chunk->generation = nextGeneration.fetch_add(1);
    chunk->bytes = sparseBytes(loaded.columns);
    chunk->columns = std::make_shared<const SparseColumns>(std::move(loaded.columns));
    if (auto it = entries_.find(key); it != entries_.end()) {
        bytes_ -= it->second.chunk->bytes;
        lru_.erase(it->second.lru);
        entries_.erase(it);
    }
    lru_.push_front(key);
    entries_[key] = {chunk, lru_.begin()};
    bytes_ += chunk->bytes;
    evictLocked();
    return chunk;
}

void ChunkStore::evictLocked() {
    // Keep at least the newest entry even when it alone exceeds the budget: the
    // caller holds it anyway, and dropping it would only force a reload.
    while (bytes_ > maxBytes_ && lru_.size() > 1) {
        const auto it = entries_.find(lru_.back());
        bytes_ -= it->second.chunk->bytes;
        entries_.erase(it);
        lru_.pop_back();
        ++stats_.evictions;
    }
}

std::shared_ptr<const StoredChunk> ChunkStore::get(const ChunkKey &key) {
    std::shared_ptr<InFlight> flight;
    bool owner = false;
    {
        std::unique_lock lock(mutex_);
        if (auto it = entries_.find(key); it != entries_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.lru);
            ++stats_.hits;
            return it->second.chunk;
        }
        ++stats_.misses;
        auto &slot = inFlight_[key];
        if (!slot) {
            slot = std::make_shared<InFlight>();
            owner = true;
        } else {
            ++stats_.sharedLoads;
        }
        flight = slot;
        if (!owner) {
            loaded_.wait(lock, [&] { return flight->done; });
            if (flight->error) std::rethrow_exception(flight->error);
            return flight->result;
        }
    }
    // Owner: load outside the lock, then publish to waiters.
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<const StoredChunk> result;
    std::exception_ptr error;
    try {
        Loaded loaded = loader_(key);
        validate(loaded.columns);
        std::scoped_lock lock(mutex_);
        ++stats_.loads;
        stats_.loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        result = insertLocked(key, std::move(loaded));
    } catch (...) {
        error = std::current_exception();
    }
    {
        std::scoped_lock lock(mutex_);
        flight->done = true;
        flight->result = result;
        flight->error = error;
        inFlight_.erase(key);
    }
    loaded_.notify_all();
    if (error) std::rethrow_exception(error);
    return result;
}

std::shared_ptr<const StoredChunk> ChunkStore::peek(const ChunkKey &key) {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    ++stats_.hits;
    return it->second.chunk;
}

std::shared_ptr<const StoredChunk> ChunkStore::cached(const ChunkKey &key) const {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second.chunk;
}

uint64_t ChunkStore::generationOf(const ChunkKey &key) const {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.chunk->generation;
}

std::shared_ptr<const StoredChunk> ChunkStore::revise(const ChunkKey &key, Loaded loaded) {
    validate(loaded.columns);
    std::scoped_lock lock(mutex_);
    ++stats_.revisions;
    return insertLocked(key, std::move(loaded));
}

std::shared_ptr<const StoredChunk> ChunkStore::reload(const ChunkKey &key) {
    const auto started = std::chrono::steady_clock::now();
    Loaded loaded = loader_(key);
    validate(loaded.columns);
    std::scoped_lock lock(mutex_);
    ++stats_.loads;
    ++stats_.revisions;
    stats_.loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return insertLocked(key, std::move(loaded));
}

ChunkStore::Stats ChunkStore::stats() const {
    std::scoped_lock lock(mutex_);
    Stats out = stats_;
    out.bytes = bytes_;
    out.entries = entries_.size();
    out.maxBytes = maxBytes_;
    return out;
}
void ChunkStore::resetStats() {
    std::scoped_lock lock(mutex_);
    stats_ = {};
}
void ChunkStore::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
}
void ChunkStore::setMaxBytes(size_t bytes) {
    std::scoped_lock lock(mutex_);
    maxBytes_ = bytes;
    evictLocked();
}
} // namespace heatmap
