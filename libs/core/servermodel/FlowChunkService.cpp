// Disk reads, 16-day assembly and compression run only on history workers.
// Invalidation is synchronous with the store fence, including cached hashes.
#include "FlowChunkService.hpp"
#include "SentinelLogging.hpp"

namespace recording {
namespace wire = protocol::chunks;
struct FlowChunkService::Cache {
    using CacheKey = std::tuple<std::string, std::string, int64_t, int64_t, uint64_t, uint16_t>;
    struct Entry { CacheKey key; Bytes bytes; wire::Frame header; FlowChunkStore::Stamp stamp; };
    std::mutex mutex;
    using BuildKey = std::tuple<std::string, int64_t, int64_t>;
    std::mutex buildsMutex;
    std::map<BuildKey, std::weak_ptr<std::mutex>> builds;
    std::mutex availabilityMutex;
    std::condition_variable availabilityReady;
    struct SnapshotEntry {
        std::pair<uint64_t, int64_t> fingerprint{};
        std::optional<FlowChunkStore::Snapshot> snapshot;
        bool building = false;
    };
    std::map<std::string, SnapshotEntry> snapshots;
    std::map<CacheKey, FlowChunkStore::Stamp> oversized; // bounded negative cache
    size_t limit;
    Stats stats;
    std::list<Entry> lru;
    std::map<CacheKey, std::list<Entry>::iterator> index;
    sentinel::metrics::Counter *hits = nullptr, *misses = nullptr, *evictions = nullptr;
    sentinel::metrics::Gauge* bytes = nullptr;
    explicit Cache(size_t budget, sentinel::metrics::MetricsRegistry* m) : limit(budget) {
        if (!m) return;
        hits = &m->counter("sentinel_flow_chunk_cache_hits_total", "Finished SCK1 frame cache hits.");
        misses = &m->counter("sentinel_flow_chunk_cache_misses_total", "Finished SCK1 frame cache misses.");
        evictions = &m->counter("sentinel_flow_chunk_cache_evictions_total", "Finished SCK1 frames removed by the byte budget.");
        bytes = &m->gauge("sentinel_flow_chunk_cache_bytes", "Finished SCK1 frame bytes resident in cache.");
    }
    static CacheKey key(const Key& k, uint64_t epoch) {
        return {k.instrument, k.channel, k.levelMs, k.startMs, epoch, wire::PayloadVersion};
    }
    void erase(std::list<Entry>::iterator it, bool budgetEviction = false) {
        stats.bytes -= it->bytes->size();
        if (budgetEviction) { ++stats.evictions; if (evictions) evictions->inc(); }
        if (bytes) bytes->set(double(stats.bytes));
        index.erase(it->key); lru.erase(it);
    }
    std::shared_ptr<std::mutex> buildLock(const Key& key) {
        std::lock_guard lock(buildsMutex);
        if (builds.size() > 1024) std::erase_if(builds, [](const auto& item) { return item.second.expired(); });
        const BuildKey id{key.instrument, key.levelMs, key.startMs};
        auto& weak = builds[id];
        if (auto held = weak.lock()) return held;
        auto fresh = std::make_shared<std::mutex>();
        weak = fresh;
        return fresh;
    }
    bool failedOversize(const Key& k, const FlowChunkStore::Stamp& stamp) {
        std::lock_guard lock(mutex);
        const auto found = oversized.find(key(k, stamp.epoch));
        return found != oversized.end() && found->second == stamp;
    }
    void rememberOversize(const Key& k, const FlowChunkStore::Stamp& stamp) {
        std::lock_guard lock(mutex);
        if (oversized.size() >= 1024) oversized.erase(oversized.begin());
        oversized[key(k, stamp.epoch)] = stamp;
    }
    void invalidate(const std::string& id, std::optional<int64_t> day) {
        std::lock_guard lock(mutex);
        std::erase_if(oversized, [&](const auto& item) {
            const auto& [instrument, channel, level, start, epoch, version] = item.first;
            return instrument == id && (!day || (start < *day + heatmap::kDayMs &&
                start + heatmap::chunkSpanMs(level) > *day));
        });
        for (auto it = lru.begin(); it != lru.end();) {
            const auto& k = it->header.key;
            if (k.instrument == id && (!day ||
                (k.startMs < *day + heatmap::kDayMs && k.startMs + heatmap::chunkSpanMs(k.levelMs) > *day)))
                erase(it++);
            else ++it;
        }
    }
    std::optional<Entry> get(const Key& k, const FlowChunkStore::Stamp& stamp) {
        std::lock_guard lock(mutex);
        const auto it = index.find(key(k, stamp.epoch));
        if (it != index.end() && it->second->stamp == stamp) {
            lru.splice(lru.begin(), lru, it->second); ++stats.hits; if (hits) hits->inc(); return *it->second;
        }
        ++stats.misses; if (misses) misses->inc(); return {};
    }
    void put(Entry entry) {
        std::lock_guard lock(mutex);
        if (entry.bytes->size() > limit) return;
        if (const auto old = index.find(entry.key); old != index.end()) erase(old->second);
        while (stats.bytes > limit - entry.bytes->size()) erase(std::prev(lru.end()), true);
        stats.bytes += entry.bytes->size(); if (bytes) bytes->set(double(stats.bytes));
        lru.push_front(std::move(entry)); index.emplace(lru.front().key, lru.begin());
    }
};
FlowChunkService::FlowChunkService(FlowChunkStore& store, size_t budget, sentinel::metrics::MetricsRegistry* metrics)
    : store_(store), cache_(std::make_shared<Cache>(budget, metrics)) {
    store_.onInvalidation([weak = std::weak_ptr<Cache>(cache_)](const auto& id, auto day) {
        if (auto cache = weak.lock()) cache->invalidate(id, day);
    });
}
FlowChunkService::Stats FlowChunkService::stats() const {
    std::lock_guard lock(cache_->mutex); return cache_->stats;
}
FlowChunkService::Bytes FlowChunkService::error(const Key& key, wire::Error code, std::string message) {
    wire::Frame f; f.kind = wire::Kind::Error; f.key = key; f.error = code;
    f.key.instrument.resize(std::min<size_t>(72, f.key.instrument.size()));
    f.key.channel.resize(std::min<size_t>(32, f.key.channel.size()));
    message.resize(std::min<size_t>(255, message.size())); f.message = std::move(message);
    return std::make_shared<const std::vector<uint8_t>>(wire::encode(f));
}
std::optional<FlowChunkStore::Snapshot> FlowChunkService::availability(const std::string& id) {
    const auto fingerprint = store_.fingerprint(id);
    std::unique_lock lock(cache_->availabilityMutex);
    auto& slot = cache_->snapshots[id];
    while (slot.building) cache_->availabilityReady.wait(lock);
    if (slot.snapshot && slot.fingerprint == fingerprint) return slot.snapshot;
    slot.building = true;
    lock.unlock();
    std::optional<FlowChunkStore::Snapshot> built;
    try { built = store_.snapshot(id); }
    catch (...) {
        lock.lock(); slot.building = false; lock.unlock(); cache_->availabilityReady.notify_all();
        throw;
    }
    lock.lock();
    slot.snapshot = built;
    if (built) slot.fingerprint = {built->serial, built->liveFromMs};
    slot.building = false;
    lock.unlock(); cache_->availabilityReady.notify_all();
    return built;
}
FlowChunkService::Reply FlowChunkService::serve(const Key& key, std::optional<uint64_t> have) {
    if (!wire::validKey(key) || (key.channel == "flow.rows" && key.priceStep != 1))
        return {error(key, wire::Error::InvalidRequest, "invalid flow chunk key"), {}};
    std::optional<FlowChunkStore::Stamp> failedVerify;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto initial = store_.stamp(key.instrument, key.levelMs, key.startMs);
        if (!initial) return {error(key, wire::Error::Unavailable, "flow chunk is not sealed"), {}};
        const auto current = [&](const auto& action) {
            return store_.ifCurrent(key.instrument, key.levelMs, key.startMs, *initial, action);
        };
        // nullopt means the key's indexed generation changed during this attempt.
        auto once = [&]() -> std::optional<Reply> {
            auto buildMutex = cache_->buildLock(key);
            std::lock_guard build(*buildMutex);
            if (!store_.verify(key.instrument, key.levelMs, key.startMs, *initial)) {
                // A file can be between atomic write and publish while its
                // indexed stamp still matches. Recheck the whole key once.
                failedVerify = *initial;
                return {};
            }
            if (cache_->failedOversize(key, *initial))
                return Reply{error(key, wire::Error::BuildFailed, "flow chunk exceeds frame limit"), *initial};
            std::optional<Cache::Entry> entry;
            if (!current([&] { entry = cache_->get(key, *initial); })) return {};
            if (!entry) {
                const auto payload = store_.read(key.instrument, key.levelMs, key.startMs);
                if (!payload) {
                    if (!current([] {})) return {};
                    return Reply{error(key, wire::Error::Unavailable, "flow chunk unavailable"), {}};
                }
                if (payload->stamp != *initial) return {};
                std::array<std::optional<Cache::Entry>, 2> built;
                for (size_t n = 0; n < built.size(); ++n) {
                    wire::Frame frame; frame.key = key; frame.epoch = payload->epoch;
                    frame.key.channel = n ? "flow.rows" : "flow.summary";
                    frame.key.priceStep = n ? uint64_t(1) << payload->stepExponent : 0;
                    frame.endMs = key.startMs + heatmap::chunkSpanMs(key.levelMs); frame.committedThroughMs = frame.endMs;
                    const auto length = n ? payload->bytes.size() : payload->summaryLength;
                    frame.payload.assign(payload->bytes.begin(), payload->bytes.begin() + ptrdiff_t(length));
                    if (beforeEncodeForTest) beforeEncodeForTest(frame);
                    frame.contentHash = wire::contentHash(frame);
                    Bytes bytes;
                    try { bytes = std::make_shared<const std::vector<uint8_t>>(wire::encode(frame)); }
                    catch (const std::invalid_argument&) {
                        if (frame.key.channel == key.channel) {
                            if (!current([] {})) return {};
                            cache_->rememberOversize(key, *initial);
                            return Reply{error(key, wire::Error::BuildFailed, "flow chunk exceeds frame limit"), *initial};
                        }
                        continue; // an oversized sibling does not fail this channel
                    }
                    std::vector<uint8_t>{}.swap(frame.payload);
                    built[n] = Cache::Entry{Cache::key(frame.key, initial->epoch), std::move(bytes),
                                           std::move(frame), *initial};
                }
                const auto wanted = key.channel == "flow.rows" ? 1u : 0u;
                entry = built[wanted];
                if (!current([&] {
                    if (built[1-wanted]) cache_->put(std::move(*built[1-wanted]));
                    cache_->put(std::move(*built[wanted]));
                })) return {};
            }
            if (have && *have == entry->header.contentHash) {
                auto header = entry->header; header.kind = wire::Kind::NotModified;
                return Reply{std::make_shared<const std::vector<uint8_t>>(wire::encode(header)), *initial};
            }
            return Reply{entry->bytes, *initial};
        };
        try { if (auto reply = once()) return *reply; }
        catch (const std::exception& e) {
            sLog_Warning("Flow chunk serving failed instrument=" << key.instrument << " start=" << key.startMs << " error=" << e.what());
            if (!current([] {})) continue;
            return {error(key, wire::Error::BuildFailed, "flow chunk build failed"), *initial};
        }
    }
    const auto latest = store_.stamp(key.instrument, key.levelMs, key.startMs);
    const auto code = !latest || (failedVerify && *latest == *failedVerify)
        ? wire::Error::Unavailable : wire::Error::Busy;
    return {error(key, code,
                  "flow chunk changed during request"), {}};
}
bool FlowChunkService::admit(const Key& key, const Reply& reply, const std::function<void()>& action) const {
    if (!reply.bytes) return false;
    if (!reply.stamp) { action(); return true; }
    return store_.ifCurrent(key.instrument, key.levelMs, key.startMs, *reply.stamp, action);
}
} // namespace recording
