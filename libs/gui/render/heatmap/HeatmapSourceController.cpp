#include "HeatmapSourceController.hpp"
#include "SentinelLogging.hpp"
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <numeric>
#include <set>
#include <tuple>

namespace heatmap {
namespace {
std::atomic<uint64_t> nextChart{1};
size_t withHeadroom(size_t bytes) { return bytes + (bytes + 9) / 10; }
bool isRetained(SpanTier tier) { return tier == SpanTier::Fallback || tier == SpanTier::RecentTf; }
template <class T> void mix(size_t &h, const T &value) {
    h ^= std::hash<T>{}(value) + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
}
size_t resolutionBytes(const ResolutionSummary &summary) {
    size_t bytes = summary.columns.capacity() * sizeof(ColumnResolution);
    for (const auto &column : summary.columns)
        for (const auto &source : column.sources)
            bytes += sizeof(SourceResolution) + source.nativeTicks.capacity() * sizeof(int64_t) +
                     source.bands.capacity() * sizeof(PriceRange);
    return bytes;
}
} // namespace

bool HeatmapBudgets::valid() const {
    return decodedChunks > 0 && spanSources > 0 && gpuPerChart > 0 && decodedChunks <= cpuCeiling &&
           spanSources <= cpuCeiling - decodedChunks;
}

size_t SpanSourceKeyHash::operator()(const SpanSourceKey &key) const {
    size_t h = std::hash<std::string>{}(key.span.symbol);
    mix(h, key.span.tfMs);
    mix(h, key.span.tile);
    mix(h, key.source);
    mix(h, key.availableStartMs);
    mix(h, key.availableEndMs);
    mix(h, key.priceScale);
    for (const auto &g : key.generations) {
        mix(h, g.source);
        mix(h, g.levelMs);
        mix(h, g.startMs);
        mix(h, g.generation);
    }
    return h;
}

SpanSourceBuildPtr buildSpanSource(const SpanSourceInput &input) {
    const auto &key = input.key;
    const int64_t start = key.span.startMs(), end = key.span.endMs();
    // SparseColumns::layer is migration metadata; identity stays the source id.
    const auto composed = tiles::composeChunks(input.chunks, key.span.symbol, key.source, key.span.tfMs, start, end);
    gpu::GpuSourceOptions options;
    options.availableStartMs = key.availableStartMs;
    options.availableEndMs = key.availableEndMs;
    auto out = std::make_shared<SpanSourceBuild>();
    out->key = key;
    out->gpu = std::make_shared<const gpu::GpuSource>(gpu::buildGpuSource(composed, options));
    out->resolution = summarizeResolution(composed, key.source, key.span.tfMs, start, end, key.priceScale);
    for (const auto &column : out->resolution.columns)
        for (const auto &source : column.sources)
            if (source.commonUnits > 0)
                out->commonUnits = out->commonUnits ? std::lcm(out->commonUnits, source.commonUnits) : source.commonUnits;
    out->bytes = size_t(out->gpu->bytes()) + resolutionBytes(out->resolution);
    return out;
}

// ------------------------------------------------------------------ cache
struct SpanSourceCache::State {
    Options options;
    Stats stats;
    tiles::ByteLru<SpanSourceKey, SpanSourceBuildPtr, SpanSourceKeyHash> lru{256ull << 20};
    std::unordered_map<SpanSourceKey, std::weak_ptr<const SpanSourceBuild>, SpanSourceKeyHash> live;
    struct Waiter {
        QPointer<QObject> context;
        Completion completion;
    };
    std::unordered_map<SpanSourceKey, std::vector<Waiter>, SpanSourceKeyHash> pending;
    void trim() {
        lru.evict();
        std::erase_if(live, [](const auto &entry) { return entry.second.expired(); });
    }
};
SpanSourceCache::SpanSourceCache(QObject *parent) : SpanSourceCache(Options{}, parent) {}
SpanSourceCache::SpanSourceCache(Options options, QObject *parent)
    : QObject(parent), state_(std::make_unique<State>()) {
    options.maxJobs = std::max<size_t>(1, options.maxJobs);
    state_->lru.setMaxBytes(options.maxBytes);
    pool_.setMaxThreadCount(std::max(1, options.threads));
    pool_.setObjectName(QStringLiteral("heatmap-span-build"));
    state_->options = std::move(options);
}
SpanSourceCache::~SpanSourceCache() { pool_.waitForDone(); }
void SpanSourceCache::setMaxBytes(size_t bytes) {
    state_->options.maxBytes = bytes;
    state_->lru.setMaxBytes(bytes);
    state_->trim();
}
SpanSourceCache::Stats SpanSourceCache::stats() const {
    auto out = state_->stats;
    out.bytes = state_->lru.bytes();
    out.entries = state_->lru.size();
    out.evictions = state_->lru.evictions();
    out.jobs = state_->pending.size();
    return out;
}
SpanSourceBuildPtr SpanSourceCache::find(const SpanSourceKey &key) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (const auto *hit = state_->lru.find(key)) {
        ++state_->stats.hits;
        return *hit;
    }
    const auto it = state_->live.find(key);
    if (it == state_->live.end()) return nullptr;
    auto build = it->second.lock();
    if (!build) return nullptr;
    ++state_->stats.hits;
    state_->lru.insert(key, build, build->bytes); // in use again: cache it
    state_->trim();
    return build;
}
bool SpanSourceCache::request(SpanSourceInput input, int priority, QObject *context, Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread() && context && context->thread() == thread());
    auto &s = *state_;
    if (const auto it = s.pending.find(input.key); it != s.pending.end()) {
        ++s.stats.sharedBuilds;
        it->second.push_back({context, std::move(completion)});
        return true;
    }
    if (s.pending.size() >= s.options.maxJobs) return false;
    s.pending[input.key].push_back({context, std::move(completion)});
    ++s.stats.builds;
    auto job = [this, input = std::move(input)] {
        SpanSourceBuildPtr build;
        QString error;
        try {
            build = buildSpanSource(input);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, key = input.key, build, error] {
            auto &s = *state_;
            auto waiters = std::move(s.pending.at(key));
            s.pending.erase(key);
            if (build) {
                s.lru.insert(key, build, build->bytes);
                s.live[key] = build;
                s.trim();
            } else {
                ++s.stats.failures;
            }
            for (auto &waiter : waiters)
                if (waiter.context) waiter.completion(build, error);
            emit settled();
        }, Qt::QueuedConnection);
    };
    if (s.options.executor) s.options.executor(std::move(job), priority);
    else pool_.start(std::move(job), priority);
    return true;
}

// ------------------------------------------------------------------ controller
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 QObject *parent)
    : HeatmapSourceController(store, fetcher, cache, Options{}, parent) {}
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 Options options, QObject *parent)
    : QObject(parent), store_(store), fetcher_(fetcher), cache_(cache), options_(options),
      chart_(nextChart.fetch_add(1)), credits_(options.gpuBytes), capacity_(std::make_shared<HeatmapCapacity>()) {
    capacity_->freeBytes.store(options_.gpuBytes);
    epoch_ = capacity_->capacityEpoch.load();
    auto changed = [this](const ChunkKey &key, quint64) {
        if (!wanted_.contains(key)) return;
        failedChunks_.erase(key);
        schedule();
    };
    connect(&fetcher_, &ChunkFetcher::chunkStored, this, changed, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::chunkRevised, this, changed, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::chunkFailed, this, [this](const ChunkKey &key, const QString &, const QString &) {
        if (!wanted_.contains(key)) return;
        failedChunks_.insert(key); // build the span without it (those buckets draw loading)
        schedule();
    }, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::availabilityChanged, this, [this](const ChunkAvailability &value) {
        if (value.symbol != symbol_) return;
        failedChunks_.clear();
        schedule();
    }, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::storeCleared, this, [this] {
        reset();
        ++serial_;
        publish();
        schedule();
    }, Qt::QueuedConnection);
    connect(&cache_, &SpanSourceCache::settled, this, [this] {
        if (refused_) schedule();
    }, Qt::QueuedConnection);
    if (options_.capacityPollMs > 0) {
        auto *timer = new QTimer(this);
        timer->setInterval(options_.capacityPollMs);
        connect(timer, &QTimer::timeout, this, &HeatmapSourceController::pollCapacity);
        timer->start();
    }
}
HeatmapSourceController::~HeatmapSourceController() { fetcher_.release(chart_); }

bool HeatmapSourceController::applyBudgets(const HeatmapBudgets &budgets, ChunkStore &store, SpanSourceCache &cache) {
    if (!budgets.valid()) return false;
    store.setMaxBytes(budgets.decodedChunks);
    cache.setMaxBytes(budgets.spanSources);
    return true;
}

void HeatmapSourceController::reset() {
    slots_.clear();
    retained_.clear();
    failedChunks_.clear();
    wanted_.clear();
    fetcher_.release(chart_);
    visibleReady_ = true;
    dirty_ = true;
}

void HeatmapSourceController::setView(const std::string &symbol, int64_t tfMs, double timeLoMs, double timeHiMs) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (symbol == symbol_ && tfMs == tfMs_ && timeLoMs == timeLoMs_ && timeHiMs == timeHiMs_) return;
    if (symbol != symbol_) {
        reset();
        ++serial_;
    } else if (tfMs != tfMs_) {
        // The previous tf's built visible spans stay as fallback until the new
        // tf's visible spans are built, then as the recent-tf tier.
        ++serial_;
        retained_.clear();
        for (const auto &[id, slot] : slots_) {
            if (id.tfMs != tfMs_ || slot.plan.rank.tier != SpanTier::Visible) continue;
            if (std::any_of(slot.sources.begin(), slot.sources.end(), [](const auto &s) { return bool(s.second.ready); }))
                retained_.push_back(id);
        }
        visibleReady_ = false;
    }
    if (symbol != symbol_ || tfMs != tfMs_)
        sLog_Data("Heatmap controller view chart=" << chart_ << " symbol=" << symbol << " tf=" << tfMs
                  << " serial=" << serial_ << " retained=" << retained_.size());
    symbol_ = symbol;
    tfMs_ = tfMs;
    timeLoMs_ = timeLoMs;
    timeHiMs_ = timeHiMs;
    sLog_Probe("heatmap.controller.view", "chart=" << chart_ << " lo=" << int64_t(timeLoMs) << " hi=" << int64_t(timeHiMs));
    schedule();
}

void HeatmapSourceController::setTickRequest(TickMode mode, int64_t units) {
    Q_ASSERT(QThread::currentThread() == thread());
    tickMode_ = mode;
    tickUnits_ = units;
    sLog_Probe("heatmap.controller.tick", "chart=" << chart_ << " mode=" << (mode == TickMode::Auto ? "auto" : "manual")
               << " units=" << units);
}

void HeatmapSourceController::setGpuBudget(size_t bytes) {
    Q_ASSERT(QThread::currentThread() == thread());
    options_.gpuBytes = bytes;
    schedule();
}

std::shared_ptr<const SpanSet> HeatmapSourceController::latestSnapshot() const {
    std::scoped_lock lock(latestMutex_);
    return latest_;
}

void HeatmapSourceController::pollCapacity() {
    const auto epoch = capacity_->capacityEpoch.load(std::memory_order_acquire);
    if (epoch == epoch_) return;
    epoch_ = epoch;
    credits_ = capacity_->freeBytes.load(std::memory_order_relaxed);
    sLog_Probe("heatmap.controller.capacity", "chart=" << chart_ << " epoch=" << epoch << " free=" << credits_);
    schedule();
}

void HeatmapSourceController::schedule() {
    if (scheduled_) return;
    scheduled_ = true;
    QMetaObject::invokeMethod(this, [this] {
        scheduled_ = false;
        reconcile();
    }, Qt::QueuedConnection);
}

std::vector<SourceAvailability> HeatmapSourceController::sources() {
    std::vector<SourceAvailability> out;
    const auto available = fetcher_.availability(symbol_);
    if (!available) return out;
    for (const auto &source : available->sources)
        if (source.latestGrid && source.latestGrid->priceScale > 0) {
            priceScale_ = source.latestGrid->priceScale;
            break;
        }
    for (const auto &source : available->sources) {
        SourceAvailability a{source.id, {}};
        int64_t oldest = INT64_MAX;
        for (const auto &level : source.levels) {
            oldest = std::min(oldest, level.oldestMs);
            a.time.endMs = std::max(a.time.endMs, level.committedThroughMs);
            if (level.levelMs == kHourMs) a.time.hourThroughMs = level.committedThroughMs;
        }
        a.time.oldestMs = oldest;
        if (a.time.endMs > a.time.oldestMs) out.push_back(std::move(a));
    }
    return out;
}

size_t HeatmapSourceController::slotBytes(const Slot &slot) const {
    size_t bytes = 0;
    for (const auto &source : slot.plan.sources) {
        const auto it = slot.sources.find(source.source);
        bytes += it != slot.sources.end() && it->second.ready ? it->second.ready->bytes : options_.sourceEstimateBytes;
    }
    return bytes;
}

void HeatmapSourceController::reconcile() {
    Q_ASSERT(QThread::currentThread() == thread() && fetcher_.thread() == thread() && cache_.thread() == thread());
    if (symbol_.empty() || tfMs_ <= 0) {
        if (dirty_) publish();
        return;
    }
    const auto available = sources();
    const SpanTier retainedTier = visibleReady_ ? SpanTier::RecentTf : SpanTier::Fallback;
    std::erase_if(retained_, [&](const SpanId &id) { return !slots_.contains(id); });
    std::vector<PlannedSpan> retained;
    for (const auto &id : retained_) {
        auto span = slots_.at(id).plan;
        span.rank = {retainedTier, 0};
        retained.push_back(std::move(span));
    }
    const auto plan = planSpans(symbol_, tfMs_, timeLoMs_, timeHiMs_, available, retained);

    // Drop what the plan no longer names.
    std::set<SpanId> planned;
    for (const auto &span : plan) planned.insert(span.id);
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (planned.contains(it->first)) { ++it; continue; }
        it = slots_.erase(it);
        dirty_ = true;
    }

    // Admission, strictly by rank. Visible and fallback always enter; the rest
    // needs GPU room and node credit of bytes + 10%. Eviction only removes
    // content ranked below what it admits, so admit/evict cannot ping-pong.
    size_t used = 0;
    for (const auto &[id, slot] : slots_) used += slotBytes(slot);
    stats_.suppressed = 0;
    bool blocked = false;
    for (const auto &span : plan) {
        if (auto it = slots_.find(span.id); it != slots_.end()) {
            auto &slot = it->second;
            if (slot.plan.rank != span.rank) dirty_ = true;
            slot.plan = span;
            std::erase_if(slot.sources, [&](const auto &entry) {
                return std::none_of(span.sources.begin(), span.sources.end(),
                                    [&](const auto &s) { return s.source == entry.first; });
            });
            continue;
        }
        if (isRetained(span.rank.tier)) continue;
        const size_t bytes = span.sources.size() * options_.sourceEstimateBytes;
        const bool guarded = span.rank.tier <= SpanTier::Fallback;
        const size_t need = guarded ? bytes : withHeadroom(bytes);
        if (!guarded && (blocked || credits_ < need)) {
            ++stats_.suppressed;
            blocked = true;
            continue;
        }
        while (used + need > options_.gpuBytes) {
            auto victim = slots_.end();
            for (auto it = slots_.begin(); it != slots_.end(); ++it) {
                const auto &rank = it->second.plan.rank;
                if (rank.tier > SpanTier::Fallback && span.rank < rank &&
                    (victim == slots_.end() || victim->second.plan.rank < rank))
                    victim = it;
            }
            if (victim == slots_.end()) break;
            sLog_Probe("heatmap.controller.evict", "chart=" << chart_ << " tile=" << victim->first.tile << " tier="
                       << spanTierName(victim->second.plan.rank.tier) << " for=" << span.id.tile);
            used -= std::min(used, slotBytes(victim->second));
            slots_.erase(victim);
            ++stats_.evictions;
            dirty_ = true;
        }
        if (!guarded && used + need > options_.gpuBytes) {
            ++stats_.suppressed;
            blocked = true;
            continue;
        }
        slots_.emplace(span.id, Slot{span, {}});
        used += bytes;
        credits_ -= std::min(credits_, bytes);
        ++stats_.admissions;
        dirty_ = true;
    }
    if (stats_.suppressed)
        sLog_Probe("heatmap.controller.suppressed", "chart=" << chart_ << " spans=" << stats_.suppressed
                   << " credits=" << credits_ << " used=" << used);

    // Chunk demand: the best rank of any span needing a key wins. Retained spans
    // are already built and need nothing.
    std::unordered_map<ChunkKey, int, ChunkKeyHash> demands;
    for (const auto &span : plan) {
        if (isRetained(span.rank.tier) || !slots_.contains(span.id)) continue;
        for (const auto &source : span.sources)
            for (const auto &key : source.chunks) {
                auto [it, inserted] = demands.emplace(key, span.rank.fetchPriority());
                if (!inserted) it->second = std::max(it->second, span.rank.fetchPriority());
            }
    }
    std::vector<ChunkKey> released;
    for (const auto &key : wanted_)
        if (!demands.contains(key)) released.push_back(key);
    if (!released.empty()) fetcher_.release(chart_, released);
    wanted_.clear();
    std::map<int, std::vector<ChunkKey>, std::greater<>> byPriority;
    for (const auto &[key, priority] : demands) {
        wanted_.insert(key);
        if (!failedChunks_.contains(key)) byPriority[priority].push_back(key);
    }
    // A cached key emits nothing: the peeks below consume it. A miss re-arms here.
    for (auto &[priority, keys] : byPriority) {
        std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
            return std::tie(a.startMs, a.source, a.levelMs) < std::tie(b.startMs, b.source, b.levelMs);
        });
        fetcher_.want(chart_, keys, priority);
    }

    // Builds, by rank. Each source of a span builds on its own once all of its
    // chunks are local (failed chunks are left out and draw loading).
    bool refused = false, visibleReady = true;
    for (const auto &span : plan) {
        const auto it = slots_.find(span.id);
        if (it == slots_.end() || isRetained(span.rank.tier)) continue;
        auto &slot = it->second;
        for (const auto &plannedSource : span.sources) {
            auto &source = slot.sources[plannedSource.source];
            SpanSourceInput input;
            input.key = {span.id, plannedSource.source, plannedSource.availableStartMs,
                         plannedSource.availableEndMs, priceScale_, {}};
            bool complete = true;
            for (const auto &key : plannedSource.chunks) {
                if (auto chunk = store_.peek(key)) {
                    input.key.generations.push_back({key.symbol, key.source, key.levelMs, key.startMs, chunk->generation});
                    input.chunks.push_back(std::move(chunk));
                } else if (!failedChunks_.contains(key)) {
                    complete = false;
                }
            }
            const bool done = source.ready || (complete && input.chunks.empty());
            if (span.rank.tier == SpanTier::Visible && !done && !source.failed) visibleReady = false;
            if (!complete || input.chunks.empty()) continue;
            std::sort(input.key.generations.begin(), input.key.generations.end());
            source.expected = input.key;
            if ((source.ready && source.ready->key == input.key) || source.pending == input.key ||
                source.failed == input.key)
                continue;
            if (auto hit = cache_.find(input.key)) {
                source.ready = std::move(hit);
                source.pending.reset();
                dirty_ = true;
                continue;
            }
            if (refused) continue;
            const auto key = input.key;
            const auto serial = serial_;
            if (cache_.request(std::move(input), span.rank.fetchPriority(), this,
                               [this, serial, key](SpanSourceBuildPtr build, const QString &error) {
                                   onBuilt(serial, key, std::move(build), error);
                               }))
                source.pending = key;
            else refused = true;
        }
    }
    refused_ = refused;
    if (!visibleReady_ && visibleReady) {
        // The new tf's view is complete: fallback spans become the recent-tf tier.
        visibleReady_ = true;
        dirty_ = true;
        if (!retained_.empty()) schedule();
    }
    if (dirty_) publish();
}

void HeatmapSourceController::onBuilt(uint64_t serial, const SpanSourceKey &key, SpanSourceBuildPtr build,
                                      const QString &error) {
    SourceSlot *found = nullptr;
    if (const auto slot = slots_.find(key.span); serial == serial_ && slot != slots_.end())
        if (const auto it = slot->second.sources.find(key.source);
            it != slot->second.sources.end() && it->second.pending == key)
            found = &it->second;
    if (!found) { // an older serial, or the slot moved on to another key
        ++stats_.staleResults;
        sLog_Probe("heatmap.controller.stale", "chart=" << chart_ << " tile=" << key.span.tile << " source=" << key.source);
        return;
    }
    auto &s = *found;
    s.pending.reset();
    if (!build) {
        s.failed = key;
        sLog_Warning("Heatmap span build failed chart=" << chart_ << " symbol=" << key.span.symbol << " tf="
                     << key.span.tfMs << " tile=" << key.span.tile << " source=" << key.source << " error=" << error);
        emit buildFailed(error);
    } else {
        s.ready = std::move(build);
        s.failed.reset();
        sLog_Probe("heatmap.controller.built", "chart=" << chart_ << " tf=" << key.span.tfMs << " tile=" << key.span.tile
                   << " source=" << key.source << " bytes=" << s.ready->bytes);
    }
    dirty_ = true;
    schedule();
}

void HeatmapSourceController::publish() {
    auto set = std::make_shared<SpanSet>();
    set->version = ++version_;
    set->serial = serial_;
    set->symbol = symbol_;
    set->tfMs = tfMs_;
    set->priceScale = priceScale_;
    set->resolution.tfMs = tfMs_;
    set->resolution.priceScale = priceScale_;
    for (const auto &[id, slot] : slots_) {
        SpanSnapshot span{id, slot.plan.rank, {}, true};
        for (const auto &planned : slot.plan.sources) {
            const auto it = slot.sources.find(planned.source);
            SpanSourceSnapshot source{planned.source, {}, false};
            if (it != slot.sources.end()) {
                source.build = it->second.ready;
                source.stale = source.build && it->second.expected && source.build->key != *it->second.expected;
            }
            span.complete = span.complete && source.build && !source.stale;
            if (source.build && id.tfMs == tfMs_) mergeResolution(set->resolution, source.build->resolution);
            span.sources.push_back(std::move(source));
        }
        // Coarsest common tick first; unbuilt sources last. Ties by source id.
        std::stable_sort(span.sources.begin(), span.sources.end(), [](const auto &a, const auto &b) {
            const int64_t x = a.build ? a.build->commonUnits : 0, y = b.build ? b.build->commonUnits : 0;
            return x != y ? x > y : a.source < b.source;
        });
        set->spans.push_back(std::move(span));
    }
    std::stable_sort(set->spans.begin(), set->spans.end(), [](const auto &a, const auto &b) { return a.rank < b.rank; });
    {
        std::scoped_lock lock(latestMutex_);
        latest_ = std::move(set);
    }
    ++stats_.publications;
    dirty_ = false;
    emit snapshotChanged();
}
} // namespace heatmap
