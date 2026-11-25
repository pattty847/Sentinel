#include "FlowChunkFetcher.hpp"
#include "SentinelLogging.hpp"
#include <QThread>
#include <chrono>
#include <tuple>
namespace sentinel::flow {
namespace w = protocol::chunks;
FlowChunkFetcher::FlowChunkFetcher(FlowChunkCache& c, w::Transport& t, QObject* p)
    : FlowChunkFetcher(c, t, Options{}, p) {}
FlowChunkFetcher::FlowChunkFetcher(FlowChunkCache& c, w::Transport& t, Options o, QObject* p)
    : QObject(p), cache_(c), transport_(t), options_(std::move(o)), timer_(new QTimer(this)) {
    qRegisterMetaType<w::Key>(); qRegisterMetaType<w::ReplyPtr>(); qRegisterMetaType<w::Availability>(); qRegisterMetaType<w::Failure>();
    if (!options_.nowMs) options_.nowMs = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
    options_.retryBaseMs = std::max(1, options_.retryBaseMs);
    options_.retryMaxMs = std::max(options_.retryBaseMs, options_.retryMaxMs);
    options_.timeoutMs = std::max(1, options_.timeoutMs);
    timer_->setSingleShot(true); connect(timer_, &QTimer::timeout, this, &FlowChunkFetcher::pump);
    connect(&t, &w::Transport::connected, this, [this] {
        if (online_) down(); online_ = true; subscribe(true);
    }, Qt::QueuedConnection);
    connect(&t, &w::Transport::disconnected, this, &FlowChunkFetcher::down, Qt::QueuedConnection);
    connect(&t, &w::Transport::hostChanged, this, &FlowChunkFetcher::hostChanged, Qt::QueuedConnection);
    connect(&t, &w::Transport::availability, this, &FlowChunkFetcher::onAvailability, Qt::QueuedConnection);
    connect(&t, &w::Transport::received, this, &FlowChunkFetcher::received, Qt::QueuedConnection);
    connect(&t, &w::Transport::failed, this, &FlowChunkFetcher::failed, Qt::QueuedConnection);
}
FlowChunkFetcher::~FlowChunkFetcher() {
    for (const auto& [id, r] : requests_) transport_.forget(id);
    transport_.subscribe({}); cache_.wanted({});
}
size_t FlowChunkFetcher::estimate(const Key& key) const {
    if (options_.estimateBytes) return std::max<size_t>(1, options_.estimateBytes(key));
    const auto e = cache_.inspect(key);
    return e ? std::max<size_t>(1, e->body->storageBytes) : flowChunkEstimate(key);
}
bool FlowChunkFetcher::want(ChartId chart, FlowChunkWant want) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (!validWant(want)) return false;
    std::set<std::string> ids{want.instrument};
    for (const auto& [id, view] : charts_) if (id != chart) ids.insert(view.instrument);
    if (ids.size() > 8) return false;
    charts_[chart] = std::move(want);
    for (auto& [key, d] : demands_)
        if (key.instrument == charts_[chart].instrument && key.channel == charts_[chart].channel) d.failed = false;
    subscribe(); replan(); return true;
}
void FlowChunkFetcher::release(ChartId chart) {
    Q_ASSERT(thread() == QThread::currentThread());
    charts_.erase(chart); subscribe(); replan();
}
void FlowChunkFetcher::subscribe(bool force) {
    std::set<std::string> ids;
    for (const auto& [id, view] : charts_) ids.insert(view.instrument);
    const std::vector<std::string> instruments(ids.begin(), ids.end());
    if (!force && subscribed_ == instruments) return;
    transport_.subscribe(instruments); subscribed_ = instruments;
}
void FlowChunkFetcher::schedule() {
    if (scheduled_) return; scheduled_ = true;
    QMetaObject::invokeMethod(this, [this] { scheduled_ = false; pump(); }, Qt::QueuedConnection);
}
bool FlowChunkFetcher::current(const Key& key) const {
    const auto e = cache_.inspect(key);
    const auto a = availability_.find(key.instrument);
    return e && !e->hidden && a != availability_.end() && e->epoch == a->second.epoch(key);
}
void FlowChunkFetcher::replan(bool countCacheHits) {
    for (auto& [key, d] : demands_) d.wanted = false;
    std::set<Key> wanted;
    for (const auto& [chart, view] : charts_) {
        const auto a = availability_.find(view.instrument); if (a == availability_.end()) continue;
        const auto heldKey = [this, &a](Key key) {
            // Only current rows substitute for summary demand. A stale or hidden
            // rows entry must not force the smaller summary to revalidate rows.
            if (key.channel == "flow.summary") {
                auto rows = key; rows.channel = "flow.rows"; rows.priceStep = 1;
                if (current(rows) && !unavailable_.contains(rows) && a->second.contains(rows)) return rows;
            }
            return key;
        };
        // Apply the same substitution before budgeting padding: a retained rows
        // allocation can be substantially larger than a summary allocation.
        const auto plan = planFlowChunks(view, a->second, [&](const Key& k) { return estimate(heldKey(k)); });
        for (auto item : plan) {
            item.key = heldKey(item.key);
            auto& d = demands_[item.key];
            if (!d.wanted || std::tie(item.tier, item.order) < std::tie(d.tier, d.order)) {
                d.tier = item.tier; d.order = item.order;
            }
            d.wanted = true; wanted.insert(item.key);
            if (current(item.key)) {
                cache_.read(item.key);
                if (countCacheHits) { ++stats_.cacheHits; ++burst_.hits; }
            }
        }
    }
    stats_.wanted = wanted.size(); cache_.wanted(std::move(wanted));
    std::erase_if(demands_, [](const auto& e) { return !e.second.wanted && !e.second.request; });
    schedule();
}
void FlowChunkFetcher::down() {
    timer_->stop(); online_ = false;
    for (const auto& [id, r] : requests_) transport_.forget(id);
    requests_.clear(); availability_.clear(); unavailable_.clear();
    stats_.inFlightChunks = stats_.inFlightBytes = 0;
    for (auto& [key, d] : demands_) { d.request = 0; d.held.reset(); d.due = 0; d.attempts = 0; d.failed = false; }
}
void FlowChunkFetcher::hostChanged() {
    Q_ASSERT(thread() == QThread::currentThread());
    const auto online = online_; down(); online_ = online;
    cache_.clear(); emit storeCleared();
}
std::optional<w::Availability> FlowChunkFetcher::availability(const std::string& id) const {
    const auto it = availability_.find(id); return it == availability_.end() ? std::nullopt : std::optional(it->second);
}
void FlowChunkFetcher::onAvailability(w::Availability a) {
    if (!online_) return;
    const auto old = availability_.find(a.instrument);
    std::erase_if(unavailable_, [&](const Key& k) {
        return k.instrument == a.instrument && a.contains(k) &&
            (old == availability_.end() || !old->second.contains(k) || old->second.epoch(k) != a.epoch(k));
    });
    for (const auto& key : cache_.apply(a)) emit chunkHidden(key);
    availability_[a.instrument] = a;
    // Retained chart wants count as current demand after a seal/epoch change.
    replan(); emit availabilityChanged(std::move(a));
}
void FlowChunkFetcher::pump() {
    Q_ASSERT(thread() == QThread::currentThread());
    timer_->stop(); if (!online_) return;
    std::erase_if(demands_, [](const auto& e) { return !e.second.wanted && !e.second.request; });
    const auto now = options_.nowMs();
    std::vector<quint64> expired;
    for (const auto& [id, r] : requests_) if (r.deadline <= now) expired.push_back(id);
    for (auto id : expired) failed({id, {}, "timeout", "chunk request deadline expired"});
    std::vector<Key> pending;
    int64_t due = INT64_MAX;
    for (const auto& [key, d] : demands_) {
        const auto a = availability_.find(key.instrument);
        if (!d.wanted || d.request || d.failed || unavailable_.contains(key) || a == availability_.end() ||
            !a->second.contains(key) || current(key)) continue;
        if (d.due > now) { due = std::min(due, d.due); continue; }
        pending.push_back(key);
    }
    std::sort(pending.begin(), pending.end(), [&](const Key& a, const Key& b) {
        const auto& x = demands_.at(a); const auto& y = demands_.at(b);
        return std::tie(x.tier, x.order, a) < std::tie(y.tier, y.order, b);
    });
    std::vector<Key> admitted;
    for (const auto& key : pending) {
        // As in heatmap, admission estimates saturate at the entire budget so
        // one decoded body bigger than it can progress; actual cache bytes do not.
        const auto bytes = std::min(estimate(key), MaxBytes);
        if (stats_.inFlightChunks == MaxChunks || bytes > MaxBytes - stats_.inFlightBytes) break;
        auto& d = demands_.at(key); d.bytes = bytes; d.held = cache_.inspect(key);
        ++stats_.inFlightChunks; stats_.inFlightBytes += bytes; admitted.push_back(key);
    }
    while (!admitted.empty()) {
        const auto first = admitted.front(); std::vector<Key> keys;
        std::vector<int64_t> starts; std::vector<std::optional<uint64_t>> hashes;
        // Only adjacent same-series keys: batching cannot promote padding ahead
        // of another chart's visible keys in the global admission order.
        auto it = admitted.begin();
        while (it != admitted.end() && it->instrument == first.instrument && it->channel == first.channel && it->levelMs == first.levelMs) {
            const auto& d = demands_.at(*it); keys.push_back(*it); starts.push_back(it->startMs);
            hashes.push_back(d.held ? std::optional(d.held->hash) : std::nullopt); ++it;
        }
        admitted.erase(admitted.begin(), it);
        const auto id = transport_.request(first, std::move(starts), std::move(hashes));
        for (const auto& k : keys) demands_.at(k).request = id;
        stats_.requested += keys.size(); burst_.requested += keys.size();
        requests_[id] = {std::move(keys), now, now + options_.timeoutMs};
    }
    for (const auto& [id, r] : requests_) due = std::min(due, r.deadline);
    if (due != INT64_MAX) timer_->start(int(std::clamp<int64_t>(due - now, 0, INT_MAX)));
    probe();
}
void FlowChunkFetcher::finish(const Key& key) {
    auto& d = demands_.at(key);
    if (const auto it = requests_.find(d.request); it != requests_.end()) {
        std::erase(it->second.keys, key);
        if (it->second.keys.empty()) { transport_.forget(d.request); requests_.erase(it); }
    }
    --stats_.inFlightChunks; stats_.inFlightBytes -= d.bytes;
    d.request = 0; d.held.reset();
}
void FlowChunkFetcher::received(quint64 id, w::ReplyPtr reply) {
    const auto r = requests_.find(id); if (r == requests_.end()) return;
    if (r->second.deadline <= options_.nowMs()) { failed({id, {}, "timeout", "late chunk reply"}); return; }
    if (!reply) { failed({id, {}, "malformed", "null chunk reply"}); return; }
    const auto& f = reply->frame; const auto key = w::requestKey(f.key);
    if (std::find(r->second.keys.begin(), r->second.keys.end(), key) == r->second.keys.end() || !w::matches(key, f.key)) {
        failed({id, {}, "malformed", "chunk reply key differs from request"}); return;
    }
    auto& d = demands_.at(key);
    const auto a = availability_.find(key.instrument);
    if (a == availability_.end() || !a->second.contains(key)) { finish(key); schedule(); return; }
    // An older queued frame is a consistent body/epoch pair, but cannot confirm
    // the new epoch. Keep the old drawn body and retry against current rules.
    if (f.epoch != a->second.epoch(key)) { failed({id, key, "busy", "epoch changed during request"}); return; }
    const auto previous = cache_.inspect(key);
    if (f.kind == w::Kind::NotModified && d.held && d.held->hash == f.contentHash &&
        d.held->body->key == f.key) {
        cache_.put(key, d.held->body, f.epoch, f.contentHash); ++stats_.notModified;
    } else if (f.kind == w::Kind::Chunk && reply->body && reply->body->key == f.key && reply->body->epoch == f.epoch) {
        cache_.put(key, reply->body, f.epoch, f.contentHash); ++stats_.bodies;
    } else { failed({id, key, "malformed", "invalid decoded chunk or not_modified without held body"}); return; }
    burst_.bytes += reply->wireBytes; burst_.chunkReplyRttSum += options_.nowMs() - r->second.start;
    d.attempts = 0; d.due = 0;
    if (previous && previous->hash != f.contentHash) emit chunkRevised(key);
    emit chunkStored(key); finish(key);
    // A newly decoded allocation replaces the cold estimate. Rebudget padding
    // immediately, even when the chart has not panned since requesting it.
    replan(false);
}
void FlowChunkFetcher::failed(w::Failure f) {
    std::vector<Key> keys;
    if (const auto it = requests_.find(f.requestId); it != requests_.end()) {
        if (it->second.deadline <= options_.nowMs()) {
            f.key.reset(); f.code = "timeout"; f.message = "chunk request deadline expired";
        }
        for (const auto& k : it->second.keys) if (!f.key || *f.key == k) keys.push_back(k);
        if (keys.empty()) keys = it->second.keys;
    } else if (!f.requestId) for (const auto& [id, r] : requests_) keys.insert(keys.end(), r.keys.begin(), r.keys.end());
    if (f.code == "malformed" && !keys.empty()) ++stats_.rejected;
    for (const auto& key : keys) {
        auto& d = demands_.at(key); finish(key);
        if ((!f.requestId && (!f.key || *f.key != key)) || f.code == "busy" || f.code == "timeout" || f.code == "client_overloaded") {
            int64_t delay = options_.retryBaseMs;
            for (unsigned n = 0; n < d.attempts && delay < options_.retryMaxMs; ++n) delay = std::min<int64_t>(2 * delay, options_.retryMaxMs);
            if (d.attempts < 32) ++d.attempts;
            d.due = options_.nowMs() + delay; ++stats_.retries;
        } else {
            d.failed = true;
            if (f.code == "unavailable") {
                unavailable_.insert(key); if (cache_.hide(key)) emit chunkHidden(key);
                d.failed = false; // a later reseal, not a timer, retries this key
            }
            static sentinel::log_throttle::Site warning;
            uint32_t suppressed = 0;
            if (warning.admit(2000, sentinel::log_throttle::nowMs(), suppressed)) sLog_Warning("Flow chunk failed req=" << f.requestId << " instrument=" << key.instrument
                << " start=" << key.startMs << " code=" << f.code << " message=" << f.message);
            emit chunkFailed(key, f.code, f.message);
        }
    }
    schedule();
}
void FlowChunkFetcher::probe() {
    if (stats_.inFlightChunks || (!burst_.requested && !burst_.hits)) return;
    const auto now = options_.nowMs();
    if (now - burst_.lastLog < 1000) return;
    sLog_Probe("flow.chunks.fetch", "wanted=" << stats_.wanted << " requested=" << burst_.requested
        << " cached=" << burst_.hits << " bytes=" << burst_.bytes << " chunkReplyRttSumMs=" << burst_.chunkReplyRttSum);
    burst_ = {0, 0, 0, 0, now};
}
}
