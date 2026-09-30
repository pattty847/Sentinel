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
bool isGuarded(SpanTier tier) { return tier <= SpanTier::Fallback; }
template <class T> void mix(size_t &h, const T &value) {
    h ^= std::hash<T>{}(value) + size_t(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
}
template <class T> size_t capacityBytes(const std::vector<T> &v) { return v.capacity() * sizeof(T); }
size_t imageBytes(const gpu::GpuSource &s) {
    return sizeof(s) + s.symbol.capacity() + s.layer.capacity() + capacityBytes(s.ticks) +
           capacityBytes(s.bucketSlots) + capacityBytes(s.columnGroups) + capacityBytes(s.groups) +
           capacityBytes(s.runs) + capacityBytes(s.rowIndex) + capacityBytes(s.entries);
}
size_t resolutionBytes(const ResolutionSummary &summary) {
    size_t bytes = capacityBytes(summary.columns);
    for (const auto &column : summary.columns) {
        bytes += capacityBytes(column.sources);
        for (const auto &source : column.sources)
            bytes += source.source.capacity() + capacityBytes(source.nativeTicks) + capacityBytes(source.bands);
    }
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

SpanSourceBuildPtr buildSpanSource(const SpanSourceInput &input, std::shared_ptr<std::atomic<int64_t>> liveBytes) {
    const auto &key = input.key;
    const int64_t start = key.span.startMs(), end = key.span.endMs();
    // SparseColumns::layer is migration metadata; identity stays the source id.
    const auto composed = tiles::composeChunks(input.chunks, key.span.symbol, key.source, key.span.tfMs, start, end);
    gpu::GpuSourceOptions options;
    options.availableStartMs = key.availableStartMs;
    options.availableEndMs = key.availableEndMs;
    auto out = std::make_shared<SpanSourceBuild>();
    out->key = key;
    auto image = std::make_unique<gpu::GpuSource>(gpu::buildGpuSource(composed, options));
    out->uploadBytes = size_t(image->bytes());
    const size_t cpu = imageBytes(*image);
    if (liveBytes) {
        liveBytes->fetch_add(int64_t(cpu));
        out->gpu = std::shared_ptr<const gpu::GpuSource>(image.release(), [liveBytes, cpu](const gpu::GpuSource *p) {
            liveBytes->fetch_sub(int64_t(cpu));
            delete p;
        });
    } else {
        out->gpu = std::move(image);
    }
    out->resolution = summarizeResolution(composed, key.source, key.span.tfMs, start, end, key.priceScale);
    for (const auto &column : out->resolution.columns)
        for (const auto &source : column.sources)
            if (source.commonUnits > 0)
                out->commonUnits = out->commonUnits ? std::lcm(out->commonUnits, source.commonUnits) : source.commonUnits;
    out->bytes = sizeof(SpanSourceBuild) + cpu + resolutionBytes(out->resolution);
    return out;
}

// ------------------------------------------------------------------ cache
struct SpanSourceCache::State {
    Options options;
    Stats stats;
    std::shared_ptr<std::atomic<int64_t>> liveBytes = std::make_shared<std::atomic<int64_t>>(0);
    tiles::ByteLru<SpanSourceKey, SpanSourceBuildPtr, SpanSourceKeyHash> lru{SIZE_MAX};
    std::unordered_map<SpanSourceKey, std::weak_ptr<const SpanSourceBuild>, SpanSourceKeyHash> live;
    struct Waiter {
        QPointer<QObject> context;
        Completion completion;
    };
    struct Job {
        size_t reserved = 0;
        std::vector<Waiter> waiters;
    };
    std::unordered_map<SpanSourceKey, Job, SpanSourceKeyHash> pending;
    struct Claim {
        size_t bytes = 0;
        unsigned count = 0;
    };
    std::unordered_map<SpanSourceKey, Claim, SpanSourceKeyHash> claims;
    size_t claimedBytes = 0, reservedBytes = 0;
    std::map<std::pair<SpanId, std::string>, Hint> hints;
    std::vector<HeatmapSourceController *> controllers;
};
SpanSourceCache::SpanSourceCache(QObject *parent) : SpanSourceCache(Options{}, parent) {}
SpanSourceCache::SpanSourceCache(Options options, QObject *parent)
    : QObject(parent), state_(std::make_shared<State>()) {
    options.maxJobs = std::max<size_t>(1, options.maxJobs);
    pool_.setMaxThreadCount(std::max(1, options.threads));
    pool_.setObjectName(QStringLiteral("heatmap-span-build"));
    state_->options = std::move(options);
}
SpanSourceCache::~SpanSourceCache() { pool_.waitForDone(); }
size_t SpanSourceCache::maxBytes() const { return state_->options.maxBytes; }
void SpanSourceCache::setMaxBytes(size_t bytes) {
    state_->options.maxBytes = bytes;
    trim();
}
size_t SpanSourceCache::pinnedBytes() const { return state_->claimedBytes + state_->reservedBytes; }
SpanSourceCache::Hint SpanSourceCache::hint(const SpanId &span, const std::string &source) const {
    const auto it = state_->hints.find({span, source});
    return it == state_->hints.end() ? Hint{} : it->second;
}
SpanSourceCache::Stats SpanSourceCache::stats() const {
    auto out = state_->stats;
    out.bytes = state_->lru.bytes();
    out.entries = state_->lru.size();
    out.jobs = state_->pending.size();
    out.claimedBytes = state_->claimedBytes;
    out.reservedBytes = state_->reservedBytes;
    out.liveBytes = state_->liveBytes->load();
    return out;
}
void SpanSourceCache::attach(HeatmapSourceController *controller) { state_->controllers.push_back(controller); }
void SpanSourceCache::detach(HeatmapSourceController *controller) { std::erase(state_->controllers, controller); }

void SpanSourceCache::trim() {
    auto &s = *state_;
    // Unclaimed LRU images fill what the pinned images leave, oldest out first.
    std::vector<std::pair<SpanSourceKey, size_t>> unclaimed; // most recent first
    size_t unclaimedBytes = 0;
    s.lru.forEach([&](const SpanSourceKey &key, const SpanSourceBuildPtr &, size_t bytes) {
        if (s.claims.contains(key)) return;
        unclaimed.emplace_back(key, bytes);
        unclaimedBytes += bytes;
    });
    const size_t pinned = s.claimedBytes + s.reservedBytes;
    for (auto it = unclaimed.rbegin(); it != unclaimed.rend() && pinned + unclaimedBytes > s.options.maxBytes; ++it) {
        s.lru.erase(it->first);
        unclaimedBytes -= it->second;
        ++s.stats.evictions;
    }
    std::erase_if(s.live, [](const auto &entry) { return entry.second.expired(); });
    if (s.claimedBytes > s.options.maxBytes && !relieving_) {
        relieving_ = true;
        QMetaObject::invokeMethod(this, [this] { relieve(); }, Qt::QueuedConnection);
    }
}

void SpanSourceCache::relieve() {
    relieving_ = false;
    auto &s = *state_;
    while (s.claimedBytes > s.options.maxBytes) {
        // The lowest-rank releasable slot of any controller goes first.
        HeatmapSourceController *owner = nullptr;
        std::optional<std::pair<SpanRank, SpanId>> worst;
        for (auto *controller : s.controllers)
            for (const auto &candidate : controller->releasable())
                if (!worst || worst->first < candidate.first) {
                    worst = candidate;
                    owner = controller;
                }
        if (!owner) {
            sLog_Probe("heatmap.cache.pressure", "claimed=" << s.claimedBytes << " max=" << s.options.maxBytes
                       << " (visible and fallback only)");
            break;
        }
        ++s.stats.pressureDrops;
        owner->dropForMemory(worst->second); // releases its claims synchronously
    }
}

std::shared_ptr<void> SpanSourceCache::claim(const SpanSourceBuildPtr &build) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!build || !build->gpu) return {};
    auto &c = state_->claims[build->key];
    if (!c.count++) {
        c.bytes = build->bytes;
        state_->claimedBytes += c.bytes;
    }
    std::weak_ptr<State> weak = state_;
    auto token = std::shared_ptr<void>(static_cast<void *>(state_.get()), [this, weak, key = build->key](void *) {
        const auto s = weak.lock();
        if (!s) return; // the cache is gone
        const auto it = s->claims.find(key);
        if (it == s->claims.end() || --it->second.count) return;
        s->claimedBytes -= it->second.bytes;
        s->claims.erase(it);
        trim();
    });
    trim();
    return token;
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
    if (!build || !build->gpu) return nullptr;
    ++state_->stats.hits;
    state_->lru.insert(key, build, build->bytes); // in use again: cache it
    trim();
    return build;
}

bool SpanSourceCache::request(SpanSourceInput input, int priority, size_t reserveBytes, QObject *context,
                              Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread() && context && context->thread() == thread());
    auto &s = *state_;
    if (const auto it = s.pending.find(input.key); it != s.pending.end()) {
        ++s.stats.sharedBuilds;
        it->second.waiters.push_back({context, std::move(completion)});
        return true;
    }
    if (s.pending.size() >= s.options.maxJobs) return false;
    auto &job = s.pending[input.key];
    job.reserved = reserveBytes;
    job.waiters.push_back({context, std::move(completion)});
    s.reservedBytes += reserveBytes;
    ++s.stats.builds;
    auto run = [this, input = std::move(input), live = s.liveBytes, before = s.options.beforeBuild] {
        SpanSourceBuildPtr build;
        QString error;
        try {
            if (before) before();
            build = buildSpanSource(input, live);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, key = input.key, build, error] {
            auto &s = *state_;
            auto job = std::move(s.pending.at(key));
            s.pending.erase(key);
            s.reservedBytes -= job.reserved;
            if (build) {
                s.lru.insert(key, build, build->bytes);
                s.live[key] = build;
                s.hints[{key.span, key.source}] = {build->bytes, build->uploadBytes};
                if (s.hints.size() > 16384) s.hints.clear(); // size hints only; rebuilt on demand
            } else {
                ++s.stats.failures;
            }
            for (auto &waiter : job.waiters)
                if (waiter.context) waiter.completion(build, error);
            trim();
            emit settled();
        }, Qt::QueuedConnection);
    };
    if (s.options.executor) s.options.executor(std::move(run), priority);
    else pool_.start(std::move(run), priority);
    return true;
}

// ------------------------------------------------------------------ controller
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 QObject *parent)
    : HeatmapSourceController(store, fetcher, cache, Options{}, parent) {}
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 Options options, QObject *parent)
    : QObject(parent), store_(store), fetcher_(fetcher), cache_(cache), options_(options),
      chart_(nextChart.fetch_add(1)), reportedFree_(options.gpuBytes), capacity_(std::make_shared<HeatmapCapacity>()) {
    epoch_ = capacity_->capacityEpoch.load();
    cache_.attach(this);
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
HeatmapSourceController::~HeatmapSourceController() {
    cache_.detach(this);
    slots_.clear(); // releases claims while the cache is alive
    fetcher_.release(chart_);
}

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
        // Builds of the old serial are dropped on completion: forget them, so a
        // return to that tf rejoins the build or takes it from the cache.
        for (auto &[id, slot] : slots_)
            for (auto &[name, source] : slot.sources) source.pending.reset();
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
    if (!reported_) reportedFree_ = bytes;
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
    auto report = capacity_->take();
    reported_ = true;
    reportedFree_ = report.freeBytes;
    for (const auto &key : report.uploaded) {
        const auto slot = slots_.find(key.span);
        if (slot == slots_.end()) continue;
        const auto it = slot->second.sources.find(key.source);
        if (it == slot->second.sources.end()) continue;
        auto &source = it->second;
        if (!source.ready || source.ready->key != key) continue; // an older version: not ours now
        source.uploaded = true;
        if (source.ready->gpu) {
            // The node holds the GPU copy; keep the metadata, release the image.
            auto light = std::make_shared<SpanSourceBuild>(*source.ready);
            light->gpu.reset();
            source.ready = std::move(light);
            source.claim.reset();
            ++stats_.releasedImages;
            dirty_ = true;
        }
    }
    if (report.lost) {
        // The node lost its GPU copies: released images are rebuilt from chunks.
        for (auto &[id, slot] : slots_)
            for (auto &[name, source] : slot.sources) {
                source.uploaded = false;
                if (source.ready && !source.ready->gpu) source.ready.reset();
            }
        dirty_ = true;
        sLog_Data("Heatmap controller chart=" << chart_ << " rebuilding released images after GPU loss");
    }
    sLog_Probe("heatmap.controller.capacity", "chart=" << chart_ << " epoch=" << epoch << " free=" << reportedFree_
               << " uploaded=" << report.uploaded.size());
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
    // Keep each level's interval: hour chunks only where hour rollups exist.
    for (const auto &source : available->sources) {
        SourceAvailability a{source.id, {}};
        int64_t oldest = INT64_MAX;
        for (const auto &level : source.levels) {
            oldest = std::min(oldest, level.oldestMs);
            a.time.endMs = std::max(a.time.endMs, level.committedThroughMs);
            if (level.levelMs == kMinuteMs) a.time.minuteOldestMs = level.oldestMs;
            if (level.levelMs == kHourMs) {
                a.time.hourOldestMs = level.oldestMs;
                a.time.hourThroughMs = level.committedThroughMs;
            }
        }
        a.time.oldestMs = oldest;
        // No minute level: no minute history anywhere.
        if (!a.time.minuteOldestMs) a.time.minuteOldestMs = INT64_MAX;
        if (a.time.endMs > a.time.oldestMs) out.push_back(std::move(a));
    }
    return out;
}

size_t HeatmapSourceController::estimate(const SpanId &span, const std::string &source, bool upload) const {
    const auto hint = cache_.hint(span, source);
    const size_t known = upload ? hint.uploadBytes : hint.bytes;
    return known ? known : options_.sourceEstimateBytes;
}
size_t HeatmapSourceController::gpuBytes(const Slot &slot) const {
    size_t bytes = 0;
    for (const auto &planned : slot.plan.sources) {
        const auto it = slot.sources.find(planned.source);
        bytes += it != slot.sources.end() && it->second.ready ? it->second.ready->uploadBytes
                                                               : estimate(slot.plan.id, planned.source, true);
    }
    return bytes;
}
size_t HeatmapSourceController::outstanding(const Slot &slot) const {
    size_t bytes = 0;
    for (const auto &planned : slot.plan.sources) {
        const auto it = slot.sources.find(planned.source);
        if (it != slot.sources.end() && it->second.uploaded) continue;
        bytes += it != slot.sources.end() && it->second.ready ? it->second.ready->uploadBytes
                                                               : estimate(slot.plan.id, planned.source, true);
    }
    return bytes;
}

std::vector<std::pair<SpanRank, SpanId>> HeatmapSourceController::releasable() const {
    std::vector<std::pair<SpanRank, SpanId>> out;
    for (const auto &[id, slot] : slots_) {
        if (isGuarded(slot.plan.rank.tier)) continue;
        if (std::any_of(slot.sources.begin(), slot.sources.end(), [](const auto &s) { return bool(s.second.claim); }))
            out.emplace_back(slot.plan.rank, id);
    }
    return out;
}
void HeatmapSourceController::dropForMemory(const SpanId &span) {
    sLog_Probe("heatmap.controller.cpu_drop", "chart=" << chart_ << " tf=" << span.tfMs << " tile=" << span.tile);
    slots_.erase(span);
    ++stats_.pressureDrops;
    dirty_ = true;
    schedule();
}

void HeatmapSourceController::setReady(SourceSlot &source, SpanSourceBuildPtr build) {
    source.claim = cache_.claim(build);
    source.ready = std::move(build);
    source.uploaded = false;
    source.failed.reset();
    dirty_ = true;
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

    // Every surviving slot takes its new plan, rank and sources before anything
    // is measured or admitted; the rest leaves.
    std::map<SpanId, const PlannedSpan *> planned;
    for (const auto &span : plan) planned.emplace(span.id, &span);
    for (auto it = slots_.begin(); it != slots_.end();) {
        const auto p = planned.find(it->first);
        if (p == planned.end()) {
            it = slots_.erase(it);
            dirty_ = true;
            continue;
        }
        auto &slot = it->second;
        const auto &span = *p->second;
        if (slot.plan.rank != span.rank) dirty_ = true;
        slot.plan = span;
        std::erase_if(slot.sources, [&](const auto &entry) {
            return std::none_of(span.sources.begin(), span.sources.end(),
                                [&](const auto &s) { return s.source == entry.first; });
        });
        ++it;
    }

    // Admission, strictly by rank. Visible and fallback always enter; the rest
    // needs GPU room, node credit of bytes + 10% and CPU tier room. Eviction
    // only removes content ranked below what it admits, so it cannot ping-pong.
    size_t used = 0, reserved = 0;
    for (const auto &[id, slot] : slots_) {
        used += gpuBytes(slot);
        reserved += outstanding(slot);
    }
    size_t credit = reportedFree_ > reserved ? reportedFree_ - reserved : 0;
    size_t cpuAdmitted = 0; // CPU estimates of spans admitted below (not yet requested)
    stats_.suppressed = 0;
    bool blocked = false;
    for (const auto &span : plan) {
        if (slots_.contains(span.id) || isRetained(span.rank.tier)) continue;
        size_t bytes = 0, cpu = 0;
        for (const auto &source : span.sources) {
            bytes += estimate(span.id, source.source, true);
            cpu += estimate(span.id, source.source, false);
        }
        const bool guarded = isGuarded(span.rank.tier);
        const size_t need = guarded ? bytes : withHeadroom(bytes);
        auto cpuOver = [&] { return cache_.pinnedBytes() + cpuAdmitted + cpu > cache_.maxBytes(); };
        if (!guarded && (blocked || credit < need)) {
            ++stats_.suppressed;
            blocked = true;
            continue;
        }
        while (used + need > options_.gpuBytes || (!guarded && cpuOver())) {
            auto victim = slots_.end();
            for (auto it = slots_.begin(); it != slots_.end(); ++it) {
                const auto &rank = it->second.plan.rank;
                if (!isGuarded(rank.tier) && span.rank < rank && (victim == slots_.end() || victim->second.plan.rank < rank))
                    victim = it;
            }
            if (victim == slots_.end()) break;
            sLog_Probe("heatmap.controller.evict", "chart=" << chart_ << " tile=" << victim->first.tile << " tier="
                       << spanTierName(victim->second.plan.rank.tier) << " for=" << span.id.tile);
            used -= std::min(used, gpuBytes(victim->second));
            const size_t freed = outstanding(victim->second);
            credit += std::min(freed, reserved);
            reserved -= std::min(freed, reserved);
            slots_.erase(victim); // releases its CPU claims
            ++stats_.evictions;
            dirty_ = true;
        }
        if (!guarded && (used + need > options_.gpuBytes || cpuOver())) {
            ++stats_.suppressed;
            blocked = true;
            continue;
        }
        slots_.emplace(span.id, Slot{span, {}});
        used += bytes;
        credit -= std::min(credit, bytes);
        cpuAdmitted += cpu;
        ++stats_.admissions;
        dirty_ = true;
    }
    if (stats_.suppressed)
        sLog_Probe("heatmap.controller.suppressed", "chart=" << chart_ << " spans=" << stats_.suppressed
                   << " credit=" << credit << " used=" << used);

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
    // A cached key emits nothing: the peeks below consume it. A miss re-arms
    // here; the store keeps wanted keys, so a delivered body is not refetched.
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
                setReady(source, std::move(hit));
                continue;
            }
            if (refused) continue;
            const auto key = input.key;
            const auto serial = serial_;
            if (cache_.request(std::move(input), span.rank.fetchPriority(), estimate(span.id, key.source, false), this,
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
    if (!found) { // an older serial, or the slot moved on to another request
        ++stats_.staleResults;
        sLog_Probe("heatmap.controller.stale", "chart=" << chart_ << " tile=" << key.span.tile << " source=" << key.source);
        return;
    }
    auto &s = *found;
    s.pending.reset();
    if (s.expected && *s.expected != key) {
        // Superseded while queued (the replacement was refused by a full queue):
        // keep the published version; reconcile requests the newest key.
        ++stats_.staleResults;
        sLog_Probe("heatmap.controller.stale", "chart=" << chart_ << " tile=" << key.span.tile << " source="
                   << key.source << " superseded");
        schedule();
        return;
    }
    if (!build) {
        s.failed = key;
        sLog_Warning("Heatmap span build failed chart=" << chart_ << " symbol=" << key.span.symbol << " tf="
                     << key.span.tfMs << " tile=" << key.span.tile << " source=" << key.source << " error=" << error);
        emit buildFailed(error);
    } else {
        sLog_Probe("heatmap.controller.built", "chart=" << chart_ << " tf=" << key.span.tfMs << " tile=" << key.span.tile
                   << " source=" << key.source << " bytes=" << build->bytes);
        setReady(s, std::move(build));
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
