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
    std::array<std::mutex, 32> builds; // serialize a chunk across summary/rows requests
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
        evictions = &m->counter("sentinel_flow_chunk_cache_evictions_total", "Finished SCK1 frames removed from cache.");
        bytes = &m->gauge("sentinel_flow_chunk_cache_bytes", "Finished SCK1 frame bytes resident in cache.");
    }
    static CacheKey key(const Key& k, uint64_t epoch) {
        return {k.instrument, k.channel, k.levelMs, k.startMs, epoch, wire::PayloadVersion};
    }
    void erase(std::list<Entry>::iterator it) {
        stats.bytes -= it->bytes->size(); ++stats.evictions;
        if (evictions) evictions->inc(); if (bytes) bytes->set(double(stats.bytes));
        index.erase(it->key); lru.erase(it);
    }
    void invalidate(const std::string& id, std::optional<int64_t> day) {
        std::lock_guard lock(mutex);
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
        while (stats.bytes > limit - entry.bytes->size()) erase(std::prev(lru.end()));
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
FlowChunkService::Reply FlowChunkService::serve(const Key& key, std::optional<uint64_t> have) {
    if (!wire::validKey(key) || (key.channel == "flow.rows" && key.priceStep != 1))
        return {error(key, wire::Error::InvalidRequest, "invalid flow chunk key"), {}};
    const auto initial = store_.stamp(key.instrument, key.levelMs, key.startMs);
    if (!initial) return {error(key, wire::Error::Unavailable, "flow chunk is not sealed"), {}};
    auto current = [&](auto action) { return store_.ifCurrent(key.instrument, key.levelMs, key.startMs, *initial, action); };
    try {
        const auto shard = (std::hash<std::string>{}(key.instrument) ^ uint64_t(key.startMs / heatmap::kHourMs) ^ uint64_t(key.levelMs / heatmap::kMinuteMs)) % cache_->builds.size();
        std::lock_guard build(cache_->builds[shard]);
        if (!store_.verify(key.instrument, key.levelMs, key.startMs, *initial))
            return {error(key, wire::Error::Unavailable, "flow chunk unavailable"), {}};
        std::optional<Cache::Entry> entry;
        if (!current([&] { entry = cache_->get(key, *initial); })) return {};
        if (!entry) {
            const auto payload = store_.read(key.instrument, key.levelMs, key.startMs);
            if (!payload) {
                if (!current([] {})) return {}; // fenced while the worker read
                return {error(key, wire::Error::Unavailable, "flow chunk unavailable"), {}};
            }
            if (payload->stamp != *initial) return {};
            // Both channels share one store read/16-day assembly. Keep finished
            // frames only; the summary always uses the stored byte prefix.
            std::array<std::optional<Cache::Entry>, 2> built;
            for (size_t n = 0; n < built.size(); ++n) {
                wire::Frame frame; frame.key = key; frame.epoch = payload->epoch;
                frame.key.channel = n ? "flow.rows" : "flow.summary";
                frame.key.priceStep = n ? uint64_t(1) << payload->stepExponent : 0;
                frame.endMs = key.startMs + heatmap::chunkSpanMs(key.levelMs); frame.committedThroughMs = frame.endMs;
                const auto length = n ? payload->bytes.size() : payload->summaryLength;
                frame.payload.assign(payload->bytes.begin(), payload->bytes.begin() + ptrdiff_t(length));
                frame.contentHash = wire::contentHash(frame);
                Bytes bytes;
                try { bytes = std::make_shared<const std::vector<uint8_t>>(wire::encode(frame)); }
                catch (const std::exception&) {
                    if (frame.key.channel == key.channel) throw;
                    continue; // a sibling exceeding the frame limit cannot fail this channel
                }
                std::vector<uint8_t>{}.swap(frame.payload);
                const auto cacheKey = Cache::key(frame.key, initial->epoch);
                built[n] = Cache::Entry{cacheKey, std::move(bytes), std::move(frame), *initial};
            }
            entry = built[key.channel == "flow.rows" ? 1 : 0];
            if (!current([&] {
                const auto wanted = key.channel == "flow.rows" ? 1u : 0u;
                if (built[1-wanted]) cache_->put(std::move(*built[1-wanted]));
                cache_->put(std::move(*built[wanted]));
            })) return {};
        }
        if (have && *have == entry->header.contentHash) {
            auto header = entry->header; header.kind = wire::Kind::NotModified;
            return {std::make_shared<const std::vector<uint8_t>>(wire::encode(header)), *initial};
        }
        return {entry->bytes, *initial};
    } catch (const std::exception& e) {
        sLog_Warning("Flow chunk serving failed instrument=" << key.instrument << " start=" << key.startMs << " error=" << e.what());
        if (!current([] {})) return {};
        return {error(key, wire::Error::BuildFailed, "flow chunk build failed"), {}};
    }
}
bool FlowChunkService::admit(const Key& key, const Reply& reply, const std::function<void()>& action) const {
    if (!reply.bytes) return false;
    if (!reply.stamp) { action(); return true; }
    return store_.ifCurrent(key.instrument, key.levelMs, key.startMs, *reply.stamp, action);
}
} // namespace recording
