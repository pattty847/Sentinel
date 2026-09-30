#include "../protocol/SentinelStreamClient.hpp" // shared-frame metatype declaration
#include "ChunkFetcher.hpp"
#include "../SentinelLogging.hpp"
#include <QThread>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <limits>

namespace heatmap {
struct ChunkFetcher::RevisionRelay {
    std::mutex mutex;
    ChunkFetcher *target = nullptr;
};
ChunkFetcher::ChunkFetcher(ChunkStore &store, ChunkTransport &transport, QObject *parent)
    : ChunkFetcher(store, transport, Options{}, parent) {}
ChunkFetcher::ChunkFetcher(ChunkStore &store, ChunkTransport &transport, Options options, QObject *parent)
    : QObject(parent), store_(store), transport_(transport), options_(std::move(options)),
      retry_(new QTimer(this)), relay_(std::make_shared<RevisionRelay>()) {
    qRegisterMetaType<ChunkKey>();
    qRegisterMetaType<OptionalChunkKey>();
    qRegisterMetaType<ChunkFramePtr>();
    qRegisterMetaType<ChunkAvailability>();
    if (!options_.nowMs) options_.nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    options_.retryBaseMs = std::max(1, options_.retryBaseMs);
    options_.retryMaxMs = std::max(options_.retryBaseMs, options_.retryMaxMs);
    options_.requestTimeoutMs = std::max(1, options_.requestTimeoutMs);
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, &ChunkFetcher::pump);
    connect(&transport_, &ChunkTransport::connected, this, [this] {
        connected_ = true;
        sLog_Data("Chunk fetcher connected; waiting for fresh availability");
    }, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::disconnected, this, &ChunkFetcher::disconnected, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::availability, this, &ChunkFetcher::onAvailability, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::received, this, &ChunkFetcher::onReceived, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::failed, this, &ChunkFetcher::onFailed, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::hostChanged, this, &ChunkFetcher::hostChanged, Qt::QueuedConnection);
    connect(&transport_, &ChunkTransport::wireVersionMismatch, this, &ChunkFetcher::wireVersionMismatch, Qt::QueuedConnection);
    relay_->target = this;
    store_.setRevisionListener([relay = relay_, &store](const ChunkKey &key) {
        std::scoped_lock lock(relay->mutex);
        if (auto *target = relay->target) {
            const auto generation = store.generationOf(key);
            QMetaObject::invokeMethod(target, [target, key, generation] {
                if (target->store_.generationOf(key) == generation)
                    emit target->chunkRevised(key, generation);
            }, Qt::QueuedConnection);
        }
    });
}
ChunkFetcher::~ChunkFetcher() {
    std::scoped_lock lock(relay_->mutex);
    relay_->target = nullptr;
    store_.setRevisionListener({});
}
void ChunkFetcher::schedule() {
    if (scheduled_) return;
    scheduled_ = true;
    QMetaObject::invokeMethod(this, [this] { scheduled_ = false; pump(); }, Qt::QueuedConnection);
}
void ChunkFetcher::want(ChartId chart, const std::vector<ChunkKey> &keys, int priority) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (const auto &key : keys) {
        auto &d = demands_[key];
        if (!d.order) d.order = ++order_;
        d.charts[chart] = priority;
        const auto cached = store_.peek(key);
        if (!d.request && (!cached || (!cached->sealed && !current_.contains(key)))) d.pending = true;
    }
    schedule();
}
void ChunkFetcher::release(ChartId chart, const std::vector<ChunkKey> &keys) {
    Q_ASSERT(QThread::currentThread() == thread());
    for (const auto &key : keys) {
        if (auto it = demands_.find(key); it != demands_.end()) it->second.charts.erase(chart);
        prune(key);
    }
    schedule();
}
void ChunkFetcher::release(ChartId chart) {
    std::vector<ChunkKey> keys;
    for (const auto &[key, d] : demands_) if (d.charts.contains(chart)) keys.push_back(key);
    release(chart, keys);
}
void ChunkFetcher::prune(const ChunkKey &key) {
    const auto it = demands_.find(key);
    if (it != demands_.end() && it->second.charts.empty() && !it->second.request) demands_.erase(it);
}
void ChunkFetcher::disconnected() {
    connected_ = false;
    retry_->stop();
    for (const auto &[id, request] : requests_) transport_.forget(id);
    requests_.clear();
    availability_.clear();
    current_.clear();
    stats_.inFlightChunks = stats_.inFlightBytes = 0;
    for (auto it = demands_.begin(); it != demands_.end();) {
        auto &d = it->second;
        if (d.charts.empty()) { it = demands_.erase(it); continue; }
        const auto cached = store_.cached(it->first);
        d.pending = d.pending || d.request || !cached || !cached->sealed;
        d.request = 0;
        d.refresh = false;
        d.held.reset();
        d.dueMs = 0;
        d.busyCount = 0;
        ++it;
    }
    sLog_Data("Chunk fetcher disconnected; retained wanted keys=" << demands_.size());
}
void ChunkFetcher::clear() {
    const bool wasConnected = connected_;
    disconnected();
    connected_ = wasConnected;
    store_.clear();
    for (auto &[key, d] : demands_) d.pending = true;
    emit storeCleared();
}
void ChunkFetcher::hostChanged() {
    clear();
    compatible_ = true;
    sLog_Data("Chunk fetcher cache cleared for host change");
}
void ChunkFetcher::wireVersionMismatch() {
    clear();
    compatible_ = false;
    sLog_Warning("Chunk fetcher cache cleared for wire version mismatch");
}
std::optional<ChunkAvailability> ChunkFetcher::availability(const std::string &symbol) const {
    const auto it = availability_.find(symbol);
    return it == availability_.end() ? std::nullopt : std::optional(it->second);
}
int64_t ChunkFetcher::committedThrough(const ChunkKey &key) const {
    const auto it = availability_.find(key.symbol);
    if (it == availability_.end()) return 0;
    for (const auto &s : it->second.sources) if (s.id == key.source)
        for (const auto &l : s.levels) if (l.levelMs == key.levelMs) return l.committedThroughMs;
    return 0;
}
bool ChunkFetcher::ready(const ChunkKey &key) const {
    // Even unavailable keys receive a terminal transport error, rather than
    // sitting pending forever. The fresh message gates the symbol, not its data.
    return connected_ && compatible_ && availability_.contains(key.symbol);
}
void ChunkFetcher::onAvailability(ChunkAvailability value) {
    if (!connected_) return;
    if (value.chunkWireVersion != kChunkWireVersion) { wireVersionMismatch(); return; }
    compatible_ = true;
    std::vector<ChunkKey> advanced;
    for (const auto &key : current_) {
        if (key.symbol != value.symbol) continue;
        for (const auto &s : value.sources) if (s.id == key.source)
            for (const auto &l : s.levels)
                if (l.levelMs == key.levelMs && l.committedThroughMs > committedThrough(key)) advanced.push_back(key);
    }
    // Also track advances while the first body for a key is in flight.
    for (auto &[key, d] : demands_) {
        if (key.symbol != value.symbol || !d.request) continue;
        for (const auto &s : value.sources) if (s.id == key.source)
            for (const auto &l : s.levels)
                if (l.levelMs == key.levelMs && l.committedThroughMs > committedThrough(key)) d.refresh = true;
    }
    sLog_Probe("chunks.fetch.availability", "symbol=" << value.symbol << " advances=" << advanced.size());
    availability_[value.symbol] = value;
    for (const auto &key : advanced) {
        current_.erase(key);
        const auto cached = store_.cached(key);
        auto it = demands_.find(key);
        if (it == demands_.end() || it->second.charts.empty() || (cached && cached->sealed)) continue;
        if (it->second.request) it->second.refresh = true;
        else it->second.pending = true;
    }
    emit availabilityChanged(std::move(value));
    schedule();
}
size_t ChunkFetcher::estimate(const ChunkKey &key) const {
    size_t bytes = 4ull << 20;
    if (options_.estimateBytes) bytes = options_.estimateBytes(key);
    else if (const auto held = store_.cached(key)) bytes = held->bytes;
    return std::clamp(bytes, size_t(1), kMaxInFlightBytes);
}
void ChunkFetcher::pump() {
    Q_ASSERT(QThread::currentThread() == thread());
    retry_->stop();
    if (!connected_ || !compatible_) return;
    const auto now = options_.nowMs();
    std::vector<quint64> expired;
    for (const auto &[id, request] : requests_)
        if (request.deadlineMs <= now) expired.push_back(id);
    for (const auto id : expired)
        onFailed(id, {}, QStringLiteral("timeout"), QStringLiteral("chunk request deadline expired"));
    std::vector<ChunkKey> pending;
    int64_t nextDue = std::numeric_limits<int64_t>::max();
    for (const auto &[key, d] : demands_) {
        if (!d.pending || d.request || d.charts.empty() || !ready(key)) continue;
        if (d.dueMs > now) { nextDue = std::min(nextDue, d.dueMs); continue; }
        pending.push_back(key);
    }
    auto priority = [](const Demand &d) {
        int best = std::numeric_limits<int>::min();
        for (const auto &[chart, rank] : d.charts) best = std::max(best, rank);
        return best;
    };
    std::sort(pending.begin(), pending.end(), [&](const auto &a, const auto &b) {
        const auto &x = demands_.at(a), &y = demands_.at(b);
        const auto px = priority(x), py = priority(y);
        return px != py ? px > py : x.order < y.order;
    });
    // Reserve capacity in global priority order before grouping by series;
    // batching a source must not let its prefetch displace another source's view.
    std::vector<ChunkKey> admitted;
    for (const auto &key : pending) {
        const auto bytes = estimate(key);
        if (stats_.inFlightChunks == kMaxInFlightChunks) break;
        // Do not fill spare bytes with lower ranks while visible work waits;
        // doing so could keep the higher-priority key blocked indefinitely.
        if (bytes > kMaxInFlightBytes - stats_.inFlightBytes) break;
        auto &d = demands_.at(key);
        d.held = store_.peek(key);
        d.estimate = bytes;
        d.pending = false;
        d.refresh = false;
        ++stats_.inFlightChunks;
        stats_.inFlightBytes += bytes;
        admitted.push_back(key);
    }
    while (!admitted.empty()) {
        const auto first = admitted.front();
        std::vector<ChunkKey> batch;
        std::vector<int64_t> starts;
        std::vector<std::optional<uint64_t>> hashes;
        for (auto it = admitted.begin(); it != admitted.end();) {
            const auto &key = *it;
            if (key.symbol != first.symbol || key.source != first.source || key.levelMs != first.levelMs ||
                batch.size() == protocol::chunkwire::kMaxStarts) { ++it; continue; }
            const auto &d = demands_.at(key);
            batch.push_back(key);
            starts.push_back(key.startMs);
            hashes.push_back(d.held ? std::optional(d.held->contentHash) : std::nullopt);
            it = admitted.erase(it);
        }
        const auto id = transport_.request(first.symbol, first.source, first.levelMs, std::move(starts), std::move(hashes));
        for (const auto &key : batch) demands_.at(key).request = id;
        requests_[id] = {std::move(batch), now + options_.requestTimeoutMs};
        ++stats_.requests;
        stats_.requestedChunks += requests_[id].keys.size();
        sLog_Probe("chunks.fetch.request", "req=" << id << " keys=" << requests_[id].keys.size()
                   << " flight=" << stats_.inFlightChunks << " bytes=" << stats_.inFlightBytes);
    }
    for (const auto &[id, request] : requests_) nextDue = std::min(nextDue, request.deadlineMs);
    if (nextDue != std::numeric_limits<int64_t>::max())
        retry_->start(int(std::min<int64_t>(nextDue - now, std::numeric_limits<int>::max())));
}
void ChunkFetcher::finish(const ChunkKey &key, Demand &d) {
    if (auto it = requests_.find(d.request); it != requests_.end()) {
        std::erase(it->second.keys, key);
        if (it->second.keys.empty()) {
            transport_.forget(d.request);
            requests_.erase(it);
        }
    }
    --stats_.inFlightChunks;
    stats_.inFlightBytes -= d.estimate;
    d.request = 0;
    d.held.reset();
}
void ChunkFetcher::onReceived(quint64 request, ChunkFramePtr frame) {
    if (!frame) { onFailed(request, {}, QStringLiteral("malformed"), QStringLiteral("null chunk reply")); return; }
    const auto it = demands_.find(frame->key);
    if (it == demands_.end() || !request || it->second.request != request) return;
    auto &d = it->second;
    sLog_Probe("chunks.fetch.received", "req=" << request << " start=" << frame->key.startMs
               << " kind=" << int(frame->kind) << " hash=" << frame->contentHash);
    try {
        std::shared_ptr<const StoredChunk> stored;
        if (frame->kind == ChunkKind::NotModified) {
            if (!d.held || d.held->contentHash != frame->contentHash)
                throw std::invalid_argument("NotModified without matching held body");
            stored = store_.cached(frame->key);
            if (!stored || stored->contentHash != frame->contentHash || stored->sealed != frame->state.sealed)
                stored = store_.put(frame->key, d.held->columns, frame->state, frame->contentHash);
            ++stats_.notModified;
        } else if (frame->kind == ChunkKind::Chunk) {
            // Aliasing pointer: the store retains the decoded frame's columns.
            auto columns = std::shared_ptr<const SparseColumns>(frame, &frame->columns);
            stored = store_.put(frame->key, std::move(columns), frame->state, frame->contentHash);
            ++stats_.bodies;
        } else throw std::invalid_argument("unexpected chunk reply kind");
        if (!stored) {
            // A newer version won insertion but was evicted in the meantime.
            // No current body can satisfy the chart yet; obtain that version.
            onFailed(request, frame->key, QStringLiteral("busy"), QStringLiteral("newer chunk body was evicted"));
            return;
        }
        current_.insert(frame->key);
        d.pending = d.refresh && !stored->sealed && stored->committedThroughMs < committedThrough(frame->key);
        d.busyCount = 0;
        d.dueMs = 0;
        emit chunkStored(frame->key, stored->generation);
        finish(frame->key, d);
        prune(frame->key);
        schedule();
    } catch (const std::exception &e) {
        onFailed(request, frame->key, QStringLiteral("malformed"), QString::fromUtf8(e.what()));
    }
}
void ChunkFetcher::onFailed(quint64 request, OptionalChunkKey key, const QString &code, const QString &message) {
    std::vector<ChunkKey> keys;
    if (const auto it = requests_.find(request); it != requests_.end()) {
        for (const auto &candidate : it->second.keys) if (!key || *key == candidate) keys.push_back(candidate);
        // Request-level refusals can echo a sentinel start (0), not a real key.
        if (keys.empty()) keys = it->second.keys;
    } else if (!request) {
        // No readable envelope id: recover the slots, but only an explicitly
        // identified key is proven bad. Retry every other key with backoff.
        for (const auto &[id, batch] : requests_) keys.insert(keys.end(), batch.keys.begin(), batch.keys.end());
    }
    for (const auto &k : keys) {
        auto &d = demands_.at(k);
        finish(k, d);
        d.pending = false;
        const bool unidentified = !request && (!key || *key != k);
        if (unidentified || code == QStringLiteral("busy") || code == QStringLiteral("client_overloaded") ||
            code == QStringLiteral("timeout")) {
            int64_t delay = options_.retryBaseMs;
            for (unsigned n = 0; n < d.busyCount && delay < options_.retryMaxMs; ++n)
                delay = std::min<int64_t>(delay * 2, options_.retryMaxMs);
            ++d.busyCount;
            d.dueMs = options_.nowMs() + delay;
            d.pending = !d.charts.empty();
            ++stats_.retries;
            sLog_Probe("chunks.fetch.busy", "req=" << request << " retryMs=" << delay);
        } else if (code != QStringLiteral("superseded")) {
            sLog_Warning("Chunk fetch failed req=" << request << " start=" << k.startMs
                         << " code=" << code << " message=" << message);
            emit chunkFailed(k, code, message);
        }
        prune(k);
    }
    schedule();
}
} // namespace heatmap
