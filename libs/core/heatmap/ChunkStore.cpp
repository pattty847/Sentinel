#include "ChunkStore.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>

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
    mix(std::hash<std::string>{}(key.source));
    mix(std::hash<int64_t>{}(key.levelMs));
    mix(std::hash<int64_t>{}(key.startMs));
    return h;
}

ChunkStore::ChunkStore(size_t maxBytes, Loader loader) : loader_(std::move(loader)), maxBytes_(maxBytes) {}

std::shared_ptr<const StoredChunk> ChunkStore::cachedLocked(const ChunkKey &key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second.chunk;
}

std::shared_ptr<const StoredChunk> ChunkStore::insertLocked(const ChunkKey &key, Loaded loaded, uint64_t ticket,
                                                            bool revision, bool *stored) {
    return insertSharedLocked(key, std::make_shared<const SparseColumns>(std::move(loaded.columns)),
                              {loaded.sealed, 0, loaded.revision}, std::nullopt, ticket, revision, stored);
}

std::shared_ptr<const StoredChunk> ChunkStore::insertSharedLocked(const ChunkKey &key,
    std::shared_ptr<const SparseColumns> columns, ChunkState state, std::optional<uint64_t> hash,
    uint64_t ticket, bool revision, bool *stored) {
    if (stored) *stored = false;
    auto &latest = latest_[key];
    if (latest.ticket > ticket) {
        // A newer acquisition already stored its version: keep it. If it was
        // evicted meanwhile, this older data must not come back as current.
        if (auto current = cachedLocked(key); current) return current;
        if (!revision && latest.sealed && state.sealed && (!hash || latest.contentHash == hash)) {
            // Same immutable content: fall through and re-cache it under the
            // newer ticket and the same generation.
            ticket = latest.ticket;
        } else {
            return nullptr;
        }
    }
    // Wire states must not regress, including when the last body was evicted.
    if (hash && latest.contentHash && latest.generation &&
        ((latest.sealed && !state.sealed) || (!latest.sealed && !state.sealed &&
         (state.revision < latest.revision || state.committedThroughMs < latest.committedThroughMs))))
        return cachedLocked(key);
    auto chunk = std::make_shared<StoredChunk>();
    chunk->key = key;
    chunk->sealed = state.sealed;
    chunk->revision = state.revision;
    chunk->committedThroughMs = state.committedThroughMs;
    chunk->contentHash = hash.value_or(0);
    // A sealed chunk never changes: re-loading it after eviction keeps its
    // generation, so nothing derived from it goes stale for no reason.
    const bool sameContent = latest.generation && (hash
        ? latest.contentHash == hash && state.sealed == latest.sealed
        : !revision && state.sealed && latest.sealed);
    chunk->generation = sameContent ? latest.generation : nextGeneration.fetch_add(1);
    chunk->ticket = ticket;
    chunk->bytes = sparseBytes(*columns);
    chunk->columns = std::move(columns);
    latest = {chunk->generation, ticket, chunk->sealed, hash, state.revision, state.committedThroughMs};
    if (auto it = entries_.find(key); it != entries_.end()) {
        bytes_ -= it->second.chunk->bytes;
        lru_.erase(it->second.lru);
        entries_.erase(it);
    }
    lru_.push_front(key);
    entries_[key] = {chunk, lru_.begin()};
    bytes_ += chunk->bytes;
    evictLocked();
    if (stored) *stored = true;
    return chunk;
}

std::shared_ptr<const StoredChunk> ChunkStore::put(const ChunkKey &key,
    std::shared_ptr<const SparseColumns> columns, ChunkState state, uint64_t contentHash) {
    uint64_t ticket;
    {
        std::scoped_lock lock(mutex_);
        ticket = ++nextTicket_;
    }
    if (!columns) throw std::invalid_argument("null heatmap chunk columns");
    validate(*columns);
    std::shared_ptr<const StoredChunk> result;
    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        const auto known = latest_.find(key);
        const uint64_t before = known == latest_.end() ? 0 : known->second.generation;
        bool stored = false;
        result = insertSharedLocked(key, std::move(columns), state, contentHash, ticket, false, &stored);
        changed = stored && before && result->generation != before;
        if (stored) ++stats_.loads;
        if (changed) ++stats_.revisions;
    }
    if (changed) notify(key);
    return result;
}

void ChunkStore::evictLocked() {
    // Preserve the blocking lab API's keep-newest exception. The async put
    // path obeys the byte cap even for a single oversized body; its returned
    // pointer and generation still remain valid after immediate eviction.
    while (bytes_ > maxBytes_ && !lru_.empty()) {
        if (lru_.size() == 1 && !latest_.at(lru_.front()).contentHash) break;
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
    uint64_t ticket = 0;
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
            ticket = ++nextTicket_;
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
    // Owner: load outside the lock, then publish to waiters. If a newer
    // revision was stored and evicted while this (older) load ran, the result
    // is discarded and the key is loaded again under a new ticket.
    std::shared_ptr<const StoredChunk> result;
    std::exception_ptr error;
    bool changed = false; // an evicted chunk came back as a different version
    try {
        for (int attempt = 0; !result; ++attempt) {
            if (attempt == 3) throw std::runtime_error("heatmap chunk kept changing during its load");
            const auto started = std::chrono::steady_clock::now();
            Loaded loaded = loader_(key);
            validate(loaded.columns);
            std::scoped_lock lock(mutex_);
            ++stats_.loads;
            stats_.loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            const auto known = latest_.find(key);
            const uint64_t before = known == latest_.end() ? 0 : known->second.generation;
            bool stored = false;
            result = insertLocked(key, std::move(loaded), ticket, false, &stored);
            // Re-reading an evicted OPEN chunk stores a new generation: charts that
            // hold tiles of the old one must hear it like any other revision. A
            // first load (before == 0) and a sealed reload (same generation) are not.
            changed = stored && before != 0 && result->generation != before;
            if (!result) ticket = ++nextTicket_;
        }
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
    if (changed) notify(key);
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
    return cachedLocked(key);
}

uint64_t ChunkStore::generationOf(const ChunkKey &key) const {
    std::scoped_lock lock(mutex_);
    const auto it = latest_.find(key);
    return it == latest_.end() ? 0 : it->second.generation;
}

std::shared_ptr<const StoredChunk> ChunkStore::revise(const ChunkKey &key, Loaded loaded) {
    validate(loaded.columns);
    std::shared_ptr<const StoredChunk> result;
    bool stored = false;
    {
        std::scoped_lock lock(mutex_);
        ++stats_.revisions;
        result = insertLocked(key, std::move(loaded), ++nextTicket_, true, &stored);
    }
    if (stored) notify(key);
    return result;
}

std::shared_ptr<const StoredChunk> ChunkStore::reload(const ChunkKey &key) {
    uint64_t ticket = 0;
    {
        std::scoped_lock lock(mutex_);
        ticket = ++nextTicket_; // newer than any load already in flight
    }
    const auto started = std::chrono::steady_clock::now();
    Loaded loaded = loader_(key);
    validate(loaded.columns);
    std::shared_ptr<const StoredChunk> result;
    bool stored = false;
    {
        std::scoped_lock lock(mutex_);
        ++stats_.loads;
        ++stats_.revisions;
        stats_.loadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        result = insertLocked(key, std::move(loaded), ticket, true, &stored);
    }
    notify(key); // even when a newer load won: charts re-check what they hold
    return result;
}

void ChunkStore::notify(const ChunkKey &key) {
    revisionCount_.fetch_add(1);
    std::function<void(const ChunkKey &)> listener;
    {
        std::scoped_lock lock(mutex_);
        listener = listener_;
    }
    if (listener) listener(key);
}

void ChunkStore::setRevisionListener(std::function<void(const ChunkKey &)> listener) {
    std::scoped_lock lock(mutex_);
    listener_ = std::move(listener);
}

uint64_t ChunkStore::revisionCount() const { return revisionCount_.load(); }

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
    latest_.clear(); // derived objects built before a clear() compare as unknown, not stale
    bytes_ = 0;
}
void ChunkStore::setMaxBytes(size_t bytes) {
    std::scoped_lock lock(mutex_);
    maxBytes_ = bytes;
    evictLocked();
}
} // namespace heatmap
