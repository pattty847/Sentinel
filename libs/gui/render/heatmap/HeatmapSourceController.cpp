#include "HeatmapSourceController.hpp"
#include "SentinelLogging.hpp"
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <ctime>
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
    mix(h, key.planHash);
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
    // An entirely incomplete first bucket (e.g. today's 1D bucket) has no
    // output scan yet. Its complete end is the rounded local cutoff, not the
    // beginning of a 64-column tile many days before recording started.
    int64_t localThrough = key.availableStartMs;
    for (const auto &chunk : input.chunks) localThrough = std::max(localThrough, chunk->committedThroughMs);
    out->completeEndMs = composed.scannedRanges.empty()
        ? std::clamp(recording::floorDiv(std::min(localThrough, key.availableEndMs), key.span.tfMs) * key.span.tfMs, start, end)
        : composed.scannedRanges.back().endMs;
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
    // Builds some chart's node uploaded: never cached again (their image lives
    // only while a claimant still holds it). Pruned with `live`.
    std::unordered_set<SpanSourceKey, SpanSourceKeyHash> uploaded;
    struct Waiter {
        QPointer<QObject> context;
        Completion completion;
    };
    struct Job {
        size_t reserved = 0;          // held until the job finishes
        std::vector<ChunkBytes> keys; // input chunks it owns until then
        std::vector<Waiter> waiters;
    };
    std::unordered_map<SpanSourceKey, Job, SpanSourceKeyHash> pending;
    struct Claim {
        size_t bytes = 0;
        unsigned count = 0;
    };
    std::unordered_map<SpanSourceKey, Claim, SpanSourceKeyHash> claims;
    size_t claimedBytes = 0, reservedBytes = 0, liveJobs = 0;
    std::map<std::pair<SpanId, std::string>, Hint> hints;
    std::vector<HeatmapSourceController *> controllers;
    // Set-based CPU ledger: each chunk counts once however many owners (chart
    // commitments, running jobs) reference it.
    struct ChunkRef {
        size_t bytes = 0;
        unsigned refs = 0;
        bool measured = false;
    };
    // The size a key counts at after `in` registers: measured replaces a hint
    // (and a measured revision); a hint only raises another hint.
    static void combine(ChunkRef &ref, const ChunkBytes &in) {
        if (in.measured) {
            ref.bytes = in.bytes;
            ref.measured = true;
        } else if (!ref.measured) {
            ref.bytes = std::max(ref.bytes, in.bytes);
        }
    }
    std::unordered_map<ChunkKey, ChunkRef, ChunkKeyHash> chunkRefs;
    size_t chunkTotal = 0;
    struct Commitment {
        std::vector<ChunkBytes> keys;
        size_t reservation = 0;
    };
    std::unordered_map<const HeatmapSourceController *, Commitment> commitments;
    size_t chartReservations = 0;
    void addRefs(const std::vector<ChunkBytes> &keys) {
        for (const auto &in : keys) {
            auto &ref = chunkRefs[in.key];
            const size_t old = ref.bytes;
            if (!ref.refs) ref = {};
            combine(ref, in);
            chunkTotal = chunkTotal - (ref.refs ? old : 0) + ref.bytes;
            ++ref.refs;
        }
    }
    void dropRefs(const std::vector<ChunkBytes> &keys) {
        for (const auto &in : keys) {
            const auto it = chunkRefs.find(in.key);
            if (it == chunkRefs.end() || --it->second.refs) continue;
            chunkTotal -= it->second.bytes;
            chunkRefs.erase(it);
        }
    }
    size_t total() const { return chunkTotal + reservedBytes + chartReservations + claimedBytes; }
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
    emit budgetsChanged();
}
size_t SpanSourceCache::cpuCeiling() const { return state_->options.cpuCeiling; }
void SpanSourceCache::setCpuCeiling(size_t bytes) {
    state_->options.cpuCeiling = bytes;
    emit budgetsChanged();
}
size_t SpanSourceCache::committedCpuBytes() const { return state_->total(); }
size_t SpanSourceCache::projectedCpuBytes(const HeatmapSourceController *self, const std::vector<ChunkBytes> &keys,
                                          size_t reservation) const {
    const auto &s = *state_;
    const auto it = s.commitments.find(self);
    const State::Commitment *old = it == s.commitments.end() ? nullptr : &it->second;
    std::unordered_set<ChunkKey, ChunkKeyHash> next;
    for (const auto &in : keys) next.insert(in.key);
    int64_t bytes = int64_t(s.total() - (old ? old->reservation : 0) + reservation);
    if (old)
        for (const auto &in : old->keys) {
            const auto ref = s.chunkRefs.find(in.key);
            if (!next.contains(in.key) && ref != s.chunkRefs.end() && ref->second.refs == 1)
                bytes -= int64_t(ref->second.bytes); // only mine, and dropped
        }
    for (const auto &in : keys) {
        const auto ref = s.chunkRefs.find(in.key);
        if (ref == s.chunkRefs.end()) {
            bytes += int64_t(in.bytes);
            continue;
        }
        auto after = ref->second;
        State::combine(after, in);
        bytes += int64_t(after.bytes) - int64_t(ref->second.bytes);
    }
    return size_t(std::max<int64_t>(bytes, 0));
}
void SpanSourceCache::commitCpu(const HeatmapSourceController *self, std::vector<ChunkBytes> keys, size_t reservation,
                                bool atKeeper) {
    auto &s = *state_;
    auto &commitment = s.commitments[self];
    // Released keys or reservation free capacity; a size change alone does not.
    std::unordered_set<ChunkKey, ChunkKeyHash> next;
    for (const auto &in : keys) next.insert(in.key);
    bool released = reservation < commitment.reservation;
    for (const auto &in : commitment.keys) released = released || !next.contains(in.key);
    s.addRefs(keys); // add first, so a key kept by this owner never drops to zero
    s.dropRefs(commitment.keys);
    commitment.keys = std::move(keys);
    s.chartReservations = s.chartReservations - commitment.reservation + reservation;
    commitment.reservation = reservation;
    if (released) freed();
    // Still over with nothing left to shed here: ask every chart to shed.
    if (s.total() > s.options.cpuCeiling && atKeeper && !overing_) {
        overing_ = true;
        QMetaObject::invokeMethod(this, [this] {
            overing_ = false;
            emit overCeiling();
        }, Qt::QueuedConnection);
    }
}
void SpanSourceCache::freed() {
    if (freeing_) return;
    freeing_ = true;
    QMetaObject::invokeMethod(this, [this] {
        freeing_ = false;
        emit capacityFreed();
    }, Qt::QueuedConnection);
}
void SpanSourceCache::resizeLiveReservation(const HeatmapSourceController *self, size_t before, size_t after,
                                           bool atKeeper) {
    const auto it = state_->commitments.find(self);
    if (it == state_->commitments.end()) return;
    Q_ASSERT(it->second.reservation >= before);
    commitCpu(self, it->second.keys, it->second.reservation - before + after, atKeeper);
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
    out.jobs = state_->pending.size() + state_->liveJobs;
    out.claimedBytes = state_->claimedBytes;
    out.reservedBytes = state_->reservedBytes;
    out.liveBytes = state_->liveBytes->load();
    return out;
}
void SpanSourceCache::attach(HeatmapSourceController *controller) { state_->controllers.push_back(controller); }
void SpanSourceCache::detach(HeatmapSourceController *controller) {
    std::erase(state_->controllers, controller);
    commitCpu(controller, {}, 0, false);
    state_->commitments.erase(controller);
}

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
    std::erase_if(s.uploaded, [&](const SpanSourceKey &key) { return !s.live.contains(key); });
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
        freed();
    });
    trim();
    return token;
}

void SpanSourceCache::released(const SpanSourceKey &key) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto &s = *state_;
    // Out of the LRU at once, even while another chart claims it: a claimant
    // keeps the build through its own reference, and the image dies with the
    // last one (it uploads or closes), never parked in the LRU.
    s.uploaded.insert(key);
    if (s.lru.erase(key)) freed();
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
    if (!state_->uploaded.contains(key)) { // in use again: cache it (unless uploaded)
        state_->lru.insert(key, build, build->bytes);
        trim();
    }
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
    if (s.pending.size() + s.liveJobs >= s.options.maxJobs) return false;
    auto &job = s.pending[input.key];
    job.reserved = reserveBytes;
    for (const auto &chunk : input.chunks)
        if (chunk) job.keys.push_back({chunk->key, chunk->bytes, true});
    s.addRefs(job.keys); // the job owns its inputs until it finishes
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
            s.dropRefs(job.keys);
            if (build) {
                s.uploaded.erase(key); // a new image (a rebuild after a loss)
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
            freed(); // its reservation and input chunks are released
            emit settled();
        }, Qt::QueuedConnection);
    };
    if (s.options.executor) s.options.executor(std::move(run), priority);
    else pool_.start(std::move(run), priority);
    return true;
}

bool SpanSourceCache::requestLive(std::vector<ChunkBytes> chunks, size_t reserveBytes, QObject *context,
                                  std::function<void()> work, std::function<void(QString)> completion) {
    auto &s = *state_;
    if (s.pending.size() + s.liveJobs >= s.options.maxJobs) return false;
    ++s.liveJobs;
    s.addRefs(chunks);
    s.reservedBytes += reserveBytes;
    auto run = [this, chunks = std::move(chunks), reserveBytes, guard = QPointer<QObject>(context),
                work = std::move(work), completion = std::move(completion)] {
        QString error;
        try { work(); } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(this, [this, chunks, reserveBytes, guard, completion, error] {
            auto &s = *state_;
            --s.liveJobs;
            s.dropRefs(chunks);
            s.reservedBytes -= reserveBytes;
            if (guard) completion(error);
            freed();
            emit settled();
        }, Qt::QueuedConnection);
    };
    if (s.options.executor) s.options.executor(std::move(run), 1'000'000);
    else pool_.start(std::move(run), 1'000'000);
    return true;
}

struct HeatmapSourceController::LiveWork {
    std::map<std::string, LiveComposer> composers; // accessed by one pool job at a time
};

// ------------------------------------------------------------------ controller
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 QObject *parent)
    : HeatmapSourceController(store, fetcher, cache, Options{}, parent) {}
HeatmapSourceController::HeatmapSourceController(ChunkStore &store, ChunkFetcher &fetcher, SpanSourceCache &cache,
                                                 Options options, QObject *parent)
    : QObject(parent), store_(store), fetcher_(fetcher), cache_(cache), options_(options),
      chart_(nextChart.fetch_add(1)), reportedFree_(options.gpuBytes), capacity_(std::make_shared<HeatmapCapacity>()) {
    if (!options_.nowMs) options_.nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    if (!options_.composeNowNs) options_.composeNowNs = &HeatmapSourceController::threadCpuNs;
    liveTimer_ = new QTimer(this);
    liveTimer_->setSingleShot(true);
    connect(liveTimer_, &QTimer::timeout, this, &HeatmapSourceController::pollLive);
    liveReleaseTimer_ = new QTimer(this);
    liveReleaseTimer_->setSingleShot(true);
    connect(liveReleaseTimer_, &QTimer::timeout, this, &HeatmapSourceController::refreshLive);
    liveWork_ = std::make_shared<LiveWork>();
    epoch_ = capacity_->capacityEpoch.load();
    cache_.attach(this);
    auto changed = [this](const ChunkKey &key, quint64) {
        if (!wanted_.contains(key) && !liveWanted_.contains(key)) return;
        if (liveWanted_.contains(key)) invalidateLive();
        failedChunks_.erase(key);
        schedule();
    };
    connect(&fetcher_, &ChunkFetcher::chunkStored, this, changed, Qt::QueuedConnection);
    // A revision matters even for a chunk no longer wanted: a built span may use it.
    connect(&fetcher_, &ChunkFetcher::chunkRevised, this, [this](const ChunkKey &key, quint64) {
        if (key.symbol != symbol_) return;
        if (liveWanted_.contains(key)) invalidateLive();
        schedule();
    }, Qt::QueuedConnection);
    connect(&fetcher_, &ChunkFetcher::liveChanged, this, [this](const QString &symbol) {
        if (symbol.toStdString() != symbol_) return;
        // Compose on arrival (after the coalescing window), not on a fixed phase:
        // frames drift through a fixed 1 s deadline and waited up to 1 s for it.
        liveCoalesceUntilMs_ = options_.nowMs() + options_.liveCoalesceMs;
        invalidateLive();
        refreshLive();
    }, Qt::QueuedConnection);
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
        if (liveDirty_) pollLive();
    }, Qt::QueuedConnection);
    // Coalesced by the cache; schedule() coalesces again (one reconcile per turn).
    connect(&cache_, &SpanSourceCache::capacityFreed, this, [this] {
        if (cpuSuppressed_) schedule();
    }, Qt::QueuedConnection);
    connect(&cache_, &SpanSourceCache::budgetsChanged, this, &HeatmapSourceController::schedule, Qt::QueuedConnection);
    // Charts already at their keeper have nothing to shed: ignoring it ends the round.
    connect(&cache_, &SpanSourceCache::overCeiling, this, [this] {
        if (slots_.size() > 1) schedule();
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

int64_t HeatmapSourceController::threadCpuNs() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool HeatmapSourceController::applyBudgets(const HeatmapBudgets &budgets, ChunkStore &store, SpanSourceCache &cache) {
    if (!budgets.valid()) return false;
    store.setMaxBytes(budgets.decodedChunks);
    cache.setMaxBytes(budgets.spanSources);
    cache.setCpuCeiling(budgets.cpuCeiling);
    return true;
}

void HeatmapSourceController::reset() {
    liveInterested_ = false;
    liveReleaseMs_.reset();
    liveReleaseTimer_->stop();
    resetLive();
    slots_.clear();
    retained_.clear();
    failedChunks_.clear();
    wanted_.clear();
    fetcher_.release(chart_);
    visibleReady_ = true;
    dirty_ = true;
    // No phantom commitment or refusal from the previous symbol.
    cache_.commitCpu(this, {}, 0, false);
    cpuRefused_.clear();
    cpuRefusedBytes_ = 0;
    cpuSuppressed_ = false;
    stats_.refused = 0;
    stats_.committedBytes = 0;
}

void HeatmapSourceController::setView(const std::string &symbol, int64_t tfMs, double timeLoMs, double timeHiMs) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (symbol == symbol_ && tfMs == tfMs_ && timeLoMs == timeLoMs_ && timeHiMs == timeHiMs_) return;
    if (symbol != symbol_) {
        reset();
        ++serial_;
    } else if (tfMs != tfMs_) {
        resetLive();
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

std::shared_ptr<const LiveSnapshot> HeatmapSourceController::latestLive() const {
    std::scoped_lock lock(latestMutex_);
    return latestLive_;
}
std::shared_ptr<const ResolutionSummary> HeatmapSourceController::latestResolution() const {
    std::scoped_lock lock(latestMutex_);
    return latestResolution_;
}
void HeatmapSourceController::mergeLatestResolution() {
    auto summary = std::make_shared<ResolutionSummary>();
    summary->tfMs = tfMs_;
    summary->priceScale = priceScale_;
    if (latest_) *summary = latest_->resolution;
    if (latestLive_ && latestLive_->serial == serial_ && summary->tfMs == latestLive_->tfMs &&
        summary->priceScale == latestLive_->resolution.priceScale) {
        auto at = summary->columns.begin();
        for (const auto &column : latestLive_->resolution.columns) {
            at = std::lower_bound(at, summary->columns.end(), column.startMs,
                                 [](const auto &c, int64_t t) { return c.startMs < t; });
            if (at == summary->columns.end() || at->startMs != column.startMs)
                at = summary->columns.insert(at, {column.startMs, {}});
            for (const auto &live : column.sources) {
                const auto history = std::find_if(at->sources.begin(), at->sources.end(),
                                                 [&](const auto &s) { return s.source == live.source; });
                if (history == at->sources.end()) at->sources.push_back(live);
                else if (history->state == BucketState::NotLoaded) *history = live;
            }
            ++at;
        }
    }
    latestResolution_ = std::move(summary);
}
void HeatmapSourceController::invalidateLive() {
    liveDirty_ = true;
}
bool HeatmapSourceController::overlapsLive(const SpanId &span) const {
    if (span.symbol != symbol_ || span.tfMs != tfMs_) return false;
    for (const auto &edge : fetcher_.live(symbol_)) {
        const auto start = liveStarts_.find(edge->source);
        if (start != liveStarts_.end() && span.endMs() > start->second && span.startMs() < edge->openEndMs)
            return true;
    }
    return false;
}
void HeatmapSourceController::setLiveBytes(size_t bytes) {
    cache_.resizeLiveReservation(this, stats_.liveBytes, bytes, slots_.size() <= 1);
    stats_.committedBytes = stats_.committedBytes - std::min(stats_.committedBytes, stats_.liveBytes) + bytes;
    stats_.liveBytes = bytes;
    if (cache_.committedCpuBytes() > cache_.cpuCeiling() && slots_.size() > 1) schedule();
}
void HeatmapSourceController::resetLive() {
    liveDirty_ = false;
    liveTimer_->stop();
    liveDueMs_ = liveCoalesceUntilMs_ = 0;
    liveStarts_.clear();
    liveUploadedEnds_.clear();
    stats_.liveUploadedSpans = 0;
    stats_.liveIntervalMs = kLiveMinIntervalMs;
    liveCosts_ = {};
    liveCostCount_ = 0;
    setLiveBytes(0);
    liveWanted_.clear();
    // An old job owns its old composer until it finishes; it cannot mutate the
    // new timeframe's cache or publish after the serial/state changes.
    liveWork_ = std::make_shared<LiveWork>();
    {
        std::scoped_lock lock(latestMutex_);
        latestLive_.reset();
        mergeLatestResolution(); // retain the history summary while live is absent
    }
    emit liveChanged();
}
void HeatmapSourceController::refreshLive() {
    const auto available = fetcher_.availability(symbol_);
    if (!available) return; // disconnect freezes the published picture and demand
    int64_t through = INT64_MAX, openEnd = 0;
    const auto currentEdges = fetcher_.live(symbol_);
    for (const auto &source : available->sources)
        for (const auto &level : source.levels) if (level.levelMs == kMinuteMs) {
            auto cutoff = level.committedThroughMs;
            for (const auto &edge : currentEdges) if (edge->source == source.id) {
                cutoff = std::max(cutoff, edge->committedThroughMs);
                openEnd = std::max(openEnd, edge->openEndMs);
            }
            through = std::min(through, cutoff);
            openEnd = std::max(openEnd, cutoff + kMinuteMs);
        }
    const bool wants = through != INT64_MAX && tfMs_ >= kMinuteMs && tfMs_ <= kDayMs && tfMs_ % kMinuteMs == 0 &&
        timeHiMs_ > recording::floorDiv(through, tfMs_) * tfMs_ &&
        timeLoMs_ < recording::floorDiv(openEnd + tfMs_ - 1, tfMs_) * tfMs_;
    if (!wants) {
        if (liveInterested_) {
            const auto now = options_.nowMs();
            if (!liveReleaseMs_) liveReleaseMs_ = now + kLiveReleaseDelayMs;
            if (now >= *liveReleaseMs_) {
                fetcher_.releaseLive(chart_);
                liveInterested_ = false;
                liveReleaseMs_.reset();
                liveReleaseTimer_->stop();
            } else liveReleaseTimer_->start(int(*liveReleaseMs_ - now));
        }
        if (!liveStarts_.empty()) { resetLive(); schedule(); }
        return;
    }
    liveReleaseMs_.reset();
    liveReleaseTimer_->stop();
    fetcher_.wantLive(chart_, symbol_);
    liveInterested_ = true;
    const auto edges = fetcher_.live(symbol_);
    std::unordered_set<ChunkKey, ChunkKeyHash> wanted;
    std::map<std::string, int64_t> starts;
    for (const auto &edge : edges) {
        if (!edge->openEndMs) continue;
        const auto floor = [this](int64_t t) { return recording::floorDiv(t, tfMs_) * tfMs_; };
        const auto edgeFloor = floor(edge->committedThroughMs ? edge->committedThroughMs : edge->openEndMs - 1);
        int64_t start = edgeFloor;
        if (!edge->minutes.empty()) start = std::min(start, floor(edge->minutes.begin()->first));
        const auto previous = liveStarts_.find(edge->source);
        const auto searchStart = previous == liveStarts_.end() ? start : std::min(start, previous->second);
        // A replacement build may be published while the node still draws an
        // older one. setReady() preserves its lower E; only the upload report
        // replaces that lower bound. Include both sides of a tile rollover.
        for (const auto &[id, slot] : slots_) {
            if (id.tfMs != tfMs_ || id.endMs() <= searchStart || id.startMs() >= edge->openEndMs) continue;
            if (slot.plan.rank.tier != SpanTier::Visible && slot.plan.rank.tier != SpanTier::Fallback) continue;
            const auto s = slot.sources.find(edge->source);
            if (s != slot.sources.end() && s->second.drawCompleteEnd)
                start = std::min(start, *s->second.drawCompleteEnd);
        }
        if (previous != liveStarts_.end()) {
            // Once a bridge is retired it cannot be pinned again by a stale
            // build or old retained finals. Only a new live state resets L.
            start = std::max(start, previous->second);
            auto acknowledged = previous->second;
            for (const auto &[key, end] : liveUploadedEnds_)
                if (key.second == edge->source && key.first.tfMs == tfMs_ &&
                    key.first.startMs() <= acknowledged && key.first.endMs() > acknowledged)
                    acknowledged = std::max(acknowledged, end);
            start = std::min(start, acknowledged);
            bool drawnAtL = false;
            for (const auto &[id, slot] : slots_) {
                if (id.tfMs != tfMs_ || id.startMs() > previous->second || id.endMs() <= previous->second ||
                    (slot.plan.rank.tier != SpanTier::Visible && slot.plan.rank.tier != SpanTier::Fallback)) continue;
                const auto s = slot.sources.find(edge->source);
                if (s != slot.sources.end() && s->second.drawCompleteEnd) drawnAtL = true;
            }
            // The node no longer has a drawable bridge. Loading there is
            // preferable to pinning every historical minute chunk indefinitely.
            if (!drawnAtL && previous->second < edgeFloor) start = edgeFloor;
        }
        // Keep at least the just-finished bucket until its span uploads, even
        // when one chart bucket is longer than the nominal two-hour cap.
        const auto lagLimit = std::max(tfMs_, std::min(kMaxLiveLagMs, kMaxLiveLagBuckets * tfMs_));
        const auto capFloor = std::max(edgeFloor, floor(edge->openEndMs - 1));
        if (capFloor - start > lagLimit) {
            sLog_Data("Heatmap live window cap chart=" << chart_ << " symbol=" << symbol_ << " source=" << edge->source
                      << " from=" << start << " to=" << capFloor << " lagLimitMs=" << lagLimit);
            start = capFloor;
        }
        starts[edge->source] = start;
    }
    // One L across the source fill passes. Different source cutoffs/upload
    // times must not let the faster source remove the slower source's bridge.
    if (!starts.empty()) {
        auto lowest = std::min_element(starts.begin(), starts.end(), [](const auto &a, const auto &b) {
            return a.second < b.second;
        })->second;
        int64_t newestFloor = lowest;
        for (const auto &edge : edges) if (edge->openEndMs)
            newestFloor = std::max(newestFloor, recording::floorDiv(
                std::max(edge->committedThroughMs, edge->openEndMs - 1), tfMs_) * tfMs_);
        const auto lagLimit = std::max(tfMs_, std::min(kMaxLiveLagMs, kMaxLiveLagBuckets * tfMs_));
        if (newestFloor - lowest > lagLimit) lowest = newestFloor;
        for (auto &[source, start] : starts) start = lowest;
    }
    if (starts != liveStarts_) { liveStarts_ = std::move(starts); invalidateLive(); }
    std::erase_if(liveUploadedEnds_, [&](const auto &entry) {
        const auto &[id, source] = entry.first;
        const auto start = liveStarts_.find(source);
        const auto slot = slots_.find(id);
        if (start == liveStarts_.end() || slot == slots_.end() || id.tfMs != tfMs_ || id.endMs() <= start->second)
            return true;
        const auto s = slot->second.sources.find(source);
        if (s == slot->second.sources.end() || !s->second.drawCompleteEnd) return true;
        for (const auto &edge : edges) if (edge->source == source) return id.startMs() >= edge->openEndMs;
        return true;
    });
    stats_.liveUploadedSpans = liveUploadedEnds_.size();
    for (const auto &edge : edges) if (const auto start = liveStarts_.find(edge->source); start != liveStarts_.end())
        for (auto t = recording::floorDiv(start->second, kHourMs) * kHourMs; t < edge->openEndMs; t += kHourMs)
            wanted.insert({symbol_, edge->source, kMinuteMs, t});
    if (wanted != liveWanted_) {
        liveWanted_ = std::move(wanted);
        invalidateLive();
        schedule(); // folds these wants into the chart's chunk and CPU ledger
    }
    pollLive();
}
void HeatmapSourceController::pollLive() {
    if (liveReleaseMs_ && options_.nowMs() >= *liveReleaseMs_) { refreshLive(); return; }
    if (!liveDirty_ || liveRunning_ || liveStarts_.empty()) return;
    const auto now = options_.nowMs();
    if (const auto due = std::max(liveDueMs_, liveCoalesceUntilMs_); now < due) {
        liveTimer_->start(int(std::min<int64_t>(due - now, INT_MAX)));
        return;
    }
    struct Input {
        std::shared_ptr<const LiveEdgeSnapshot> edge;
        std::vector<std::shared_ptr<const StoredChunk>> chunks;
        int64_t start = 0;
    };
    std::vector<Input> inputs;
    std::vector<ChunkBytes> keys;
    size_t reservation = stats_.liveBytes;
    for (const auto &edge : fetcher_.live(symbol_)) {
        const auto start = liveStarts_.find(edge->source);
        if (!edge->openEndMs || start == liveStarts_.end() || edge->openEndMs <= start->second) continue;
        Input input{edge, {}, start->second};
        for (const auto &key : liveWanted_) if (key.source == edge->source)
            if (auto chunk = store_.cached(key)) {
                keys.push_back({key, chunk->bytes, true});
                input.chunks.push_back(std::move(chunk));
            }
        std::sort(input.chunks.begin(), input.chunks.end(), [](const auto &a, const auto &b) {
            return a->key.startMs < b->key.startMs;
        });
        for (const auto &[t, column] : edge->minutes) {
            Q_UNUSED(t);
            for (const auto &n : column->native)
                reservation += n.entries.size() * (sizeof(SparseEntry) + sizeof(long double)) +
                               (n.coverage[0].size() + n.coverage[1].size()) * sizeof(CoverageRun);
        }
        inputs.push_back(std::move(input));
    }
    if (inputs.empty()) return;
    struct Result {
        std::shared_ptr<LiveSnapshot> snapshot;
        double ms = 0;
        uint64_t buckets = 0, pieces = 0;
        size_t bytes = 0;
    };
    auto result = std::make_shared<Result>();
    const auto serial = serial_;
    const auto tf = tfMs_;
    const auto scale = priceScale_;
    const auto symbol = symbol_;
    auto work = [inputs = std::move(inputs), state = liveWork_, result, tf, scale, symbol, serial,
                 clock = options_.composeNowNs] {
        const auto begin = clock();
        auto out = std::make_shared<LiveSnapshot>();
        out->serial = serial;
        out->symbol = symbol;
        out->tfMs = tf;
        out->resolution.tfMs = tf;
        out->resolution.priceScale = scale;
        std::erase_if(state->composers, [&](const auto &entry) {
            return std::none_of(inputs.begin(), inputs.end(), [&](const auto &in) { return in.edge->source == entry.first; });
        });
        for (const auto &input : inputs) {
            auto composed = state->composers[input.edge->source].compose(*input.edge, input.chunks, tf, input.start);
            result->buckets += composed.composedBuckets;
            result->pieces += composed.committedPieces;
            LiveSourceSnapshot source;
            source.source = input.edge->source;
            source.revision = input.edge->revision;
            source.startMs = input.start;
            source.openEndMs = input.edge->openEndMs;
            source.columns = std::make_shared<SparseColumns>(std::move(composed.columns));
            source.gpu = std::make_shared<gpu::GpuSource>(gpu::buildGpuSource(*source.columns));
            auto summary = summarizeResolution(*source.columns, source.source, tf, source.startMs,
                                               source.columns->endMs, scale);
            for (const auto &column : summary.columns)
                for (const auto &s : column.sources) if (s.commonUnits > 0)
                    source.commonUnits = source.commonUnits ? std::lcm(source.commonUnits, s.commonUnits) : s.commonUnits;
            mergeResolution(out->resolution, summary);
            result->bytes += sparseBytes(*source.columns) + imageBytes(*source.gpu) +
                             state->composers[input.edge->source].bytes();
            out->sources.push_back(std::move(source));
        }
        result->bytes += resolutionBytes(out->resolution);
        std::sort(out->sources.begin(), out->sources.end(), [](const auto &a, const auto &b) {
            return a.commonUnits != b.commonUnits ? a.commonUnits > b.commonUnits : a.source < b.source;
        });
        result->ms = double(clock() - begin) / 1e6;
        result->snapshot = std::move(out);
    };
    auto done = [this, result, serial, started = now, state = liveWork_](const QString &error) {
        liveRunning_ = false;
        stats_.liveComposeMs = result->ms;
        stats_.liveComposedBuckets += result->buckets;
        stats_.liveCommittedPieces += result->pieces;
        // Measure the whole worker update (compose + upload image + summary) in
        // the worker's CPU time, and decide on the median of the last three
        // updates (fewer at first: the one, then the lower of two), so neither a
        // descheduled worker nor one slow update switches to 5 s.
        if (state == liveWork_ && error.isEmpty()) {
            std::rotate(liveCosts_.begin(), liveCosts_.begin() + 1, liveCosts_.end());
            liveCosts_.back() = result->ms;
            liveCostCount_ = std::min<size_t>(liveCostCount_ + 1, liveCosts_.size());
            std::array<double, 3> recent = liveCosts_;
            std::sort(recent.end() - liveCostCount_, recent.end());
            const double cost = liveCostCount_ == 3 ? recent[1] : recent[3 - liveCostCount_];
            const int interval = cost > 5 ? kLiveBackoffIntervalMs : kLiveMinIntervalMs;
            if (interval != stats_.liveIntervalMs) {
                if (interval == kLiveBackoffIntervalMs)
                    sLog_Warning("Heatmap live compose backoff chart=" << chart_ << " symbol=" << symbol_
                                 << " tf=" << tfMs_ << " ms=" << result->ms << " medianMs=" << cost
                                 << " interval=" << interval);
                else
                    sLog_Data("Heatmap live compose recovered chart=" << chart_ << " symbol=" << symbol_
                              << " tf=" << tfMs_ << " ms=" << result->ms << " interval=" << interval);
                stats_.liveIntervalMs = interval;
            }
        }
        sLog_Probe("heatmap.live.compose", "chart=" << chart_ << " symbol=" << symbol_ << " tf=" << tfMs_
                   << " ms=" << result->ms << " buckets=" << result->buckets << " committed=" << result->pieces
                   << " interval=" << stats_.liveIntervalMs);
        if (serial != serial_ || state != liveWork_) {
            ++stats_.liveStaleResults;
            pollLive(); // a different chart serial/state needs its own first picture
            return;
        }
        liveDueMs_ = started + stats_.liveIntervalMs;
        if (!error.isEmpty()) {
            sLog_Warning("Heatmap live build failed chart=" << chart_ << " symbol=" << symbol_ << " error=" << error);
            emit buildFailed(error);
            liveDirty_ = true;
            pollLive();
            return;
        }
        result->snapshot->version = ++liveVersion_;
        result->snapshot->publishedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        setLiveBytes(result->bytes);
        {
            std::scoped_lock lock(latestMutex_);
            latestLive_ = result->snapshot;
            mergeLatestResolution();
        }
        ++stats_.livePublications;
        emit liveChanged();
        // Inputs that changed during this job are still dirty. Its consistent
        // result is drawable now; the newest inputs run at the next cadence.
        pollLive();
    };
    if (cache_.requestLive(std::move(keys), reservation, this, std::move(work), std::move(done))) {
        liveDirty_ = false;
        liveRunning_ = true;
        liveDueMs_ = now + stats_.liveIntervalMs;
    } // a full pool retries on settled()
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
        source.drawMissing = false;
        if (slot->second.plan.rank.tier == SpanTier::Visible || slot->second.plan.rank.tier == SpanTier::Fallback) {
            source.drawCompleteEnd = source.ready->completeEndMs;
            if (overlapsLive(key.span)) {
                liveUploadedEnds_[{key.span, key.source}] = source.ready->completeEndMs;
                invalidateLive();
            }
        }
        if (source.ready->gpu) {
            // The node holds the GPU copy; keep the metadata, release the image.
            auto light = std::make_shared<SpanSourceBuild>(*source.ready);
            light->gpu.reset();
            source.ready = std::move(light);
            source.claim.reset();
            cache_.released(key); // and out of the LRU, unless another chart still claims it
            ++stats_.releasedImages;
            dirty_ = true;
        }
    }
    // Single sources the node no longer holds (its cap evicted them) or never
    // held (a new node): as a loss, for those keys only.
    for (const auto &key : report.missing) {
        liveUploadedEnds_.erase({key.span, key.source});
        const auto slot = slots_.find(key.span);
        if (slot == slots_.end()) continue;
        const auto it = slot->second.sources.find(key.source);
        if (it == slot->second.sources.end() || !it->second.ready || it->second.ready->key != key) continue;
        auto &source = it->second;
        source.uploaded = false;
        source.drawMissing = true;
        source.drawCompleteEnd.reset();
        if (overlapsLive(key.span)) invalidateLive();
        dirty_ = true;
        if (source.ready->gpu) continue; // the image is still here: the node uploads it again
        if (slot->second.plan.rank.tier == SpanTier::RecentTf) {
            slots_.erase(slot);
            continue;
        }
        source.ready.reset();
        source.lostRebuild = isRetained(slot->second.plan.rank.tier);
    }
    if (!report.missing.empty())
        sLog_Probe("heatmap.controller.missing", "chart=" << chart_ << " sources=" << report.missing.size());
    if (report.lost) {
        liveUploadedEnds_.clear();
        // The node lost its GPU copies. Released images of drawn content are
        // rebuilt from chunks (visible, prefetch, and fallback, which is still
        // drawn); recent-tf retention of a released image is dropped instead.
        for (auto it = slots_.begin(); it != slots_.end();) {
            auto &slot = it->second;
            const bool released = std::any_of(slot.sources.begin(), slot.sources.end(), [](const auto &s) {
                return s.second.ready && !s.second.ready->gpu;
            });
            if (released && slot.plan.rank.tier == SpanTier::RecentTf) {
                it = slots_.erase(it);
                continue;
            }
            for (auto &[name, source] : slot.sources) {
                source.uploaded = false;
                source.drawMissing = true;
                source.drawCompleteEnd.reset();
                if (overlapsLive(it->first)) invalidateLive();
                if (!source.ready || source.ready->gpu) continue;
                source.ready.reset();
                source.lostRebuild = isRetained(slot.plan.rank.tier);
            }
            ++it;
        }
        dirty_ = true;
        sLog_Data("Heatmap controller chart=" << chart_ << " rebuilding released images after GPU loss");
    }
    sLog_Probe("heatmap.controller.capacity", "chart=" << chart_ << " epoch=" << epoch << " free=" << reportedFree_
               << " uploaded=" << report.uploaded.size());
    refreshLive(); // retire missing bridges before their replacement jobs can finish
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
        for (const auto &edge : fetcher_.live(symbol_))
            if (edge->source == source.id) a.time.endMs = std::max(a.time.endMs, edge->committedThroughMs);
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
    const auto slot = slots_.find(build->key.span);
    if (!source.drawMissing && slot != slots_.end() &&
        (slot->second.plan.rank.tier == SpanTier::Visible || slot->second.plan.rank.tier == SpanTier::Fallback))
        source.drawCompleteEnd = source.drawCompleteEnd
            ? std::min(*source.drawCompleteEnd, build->completeEndMs) : build->completeEndMs;
    source.ready = std::move(build);
    source.uploaded = false;
    source.lostRebuild = false;
    source.failed.reset();
    dirty_ = true;
    if (overlapsLive(source.ready->key.span)) invalidateLive();
}

size_t HeatmapSourceController::chunkCost(const ChunkKey &key) const {
    const auto it = chunkBytes_.find(key);
    return it != chunkBytes_.end() ? it->second : options_.chunkEstimateBytes;
}

HeatmapSourceController::SourceNeed HeatmapSourceController::need(const Slot &slot, const SpanSourcePlan &planned) {
    SourceNeed out;
    auto &key = out.input.key;
    key = {slot.plan.id, planned.source, planned.availableStartMs, planned.availableEndMs, priceScale_, 0, {}};
    std::vector<ChunkKey> keys; // the chunks a build uses; failed ones are left out
    for (const auto &chunkKey : planned.chunks) {
        if (auto chunk = store_.cached(chunkKey)) {
            if (chunkBytes_.size() > 65536) chunkBytes_.clear(); // size hints only
            chunkBytes_[chunkKey] = chunk->bytes;
            key.generations.push_back({chunkKey.symbol, chunkKey.source, chunkKey.levelMs, chunkKey.startMs,
                                       chunk->generation, chunk->sealed});
            out.input.chunks.push_back(std::move(chunk));
            keys.push_back(chunkKey);
        } else if (!failedChunks_.contains(chunkKey)) {
            out.complete = false;
            keys.push_back(chunkKey);
        }
    }
    std::sort(key.generations.begin(), key.generations.end());
    uint64_t plan = 1469598103934665603ull; // FNV-1a over the full planned chunk-key list
    auto fold = [&](uint64_t v) { plan = (plan ^ v) * 1099511628211ull; };
    for (const auto &k : planned.chunks) { // failures do not change the plan
        fold(std::hash<std::string>{}(k.source));
        fold(uint64_t(k.levelMs));
        fold(uint64_t(k.startMs));
    }
    key.planHash = plan;
    const auto it = slot.sources.find(planned.source);
    const SourceSlot *source = it == slot.sources.end() ? nullptr : &it->second;
    if (isRetained(slot.plan.rank.tier) && !(source && source->lostRebuild)) return out; // built, drawn as is
    const SpanSourceBuild *ready = source && source->ready ? source->ready.get() : nullptr;
    out.build = !ready;
    auto usedBy = [](const SpanSourceKey &k, const ChunkKey &c) {
        return std::find_if(k.generations.begin(), k.generations.end(), [&](const auto &g) {
            return g.source == c.source && g.levelMs == c.levelMs && g.startMs == c.startMs;
        });
    };
    if (ready) {
        const auto &have = ready->key;
        // A changed plan (bounds, scale, the full planned chunk list) needs a
        // build, even while some of its new inputs are still missing.
        out.build = have.span != key.span || have.availableStartMs != key.availableStartMs ||
                    have.availableEndMs != key.availableEndMs || have.priceScale != key.priceScale ||
                    have.planHash != key.planHash;
        // So does a local chunk the ready build lacks or has at another
        // generation (a revision, or a chunk that failed before and arrived).
        for (const auto &g : key.generations) {
            if (out.build) break;
            const auto match = usedBy(have, {g.symbol, g.source, g.levelMs, g.startMs});
            out.build = match == have.generations.end() || match->generation != g.generation;
        }
    }
    out.desired = key;
    if (out.build) {
        out.want = std::move(keys);
    } else {
        // Built: only open chunks stay wanted (their revisions must arrive);
        // sealed ones become evictable and are wanted again for a rebuild.
        for (const auto &g : ready->key.generations)
            if (!g.sealed) out.want.push_back({g.symbol, g.source, g.levelMs, g.startMs});
        // Failure retry, separate from the plan: a planned chunk the build
        // lacks is fetched again; it rebuilds only once it arrives.
        for (const auto &k : keys)
            if (!store_.cached(k) && usedBy(ready->key, k) == ready->key.generations.end()) out.want.push_back(k);
    }
    return out;
}

void HeatmapSourceController::reconcile() {
    Q_ASSERT(QThread::currentThread() == thread() && fetcher_.thread() == thread() && cache_.thread() == thread());
    ++stats_.reconciles;
    if (symbol_.empty() || tfMs_ <= 0) {
        cache_.commitCpu(this, {}, 0, false);
        stats_.committedBytes = 0;
        if (dirty_) publish();
        if (!symbol_.empty()) refreshLive();
        return;
    }
    const auto available = sources();
    // The time this tf can plan spans for: minute history, and hour rollups for
    // hour-multiple timeframes (tiles::chunksFor), not an earlier level the tf
    // never reads (the node would wait for spans that are never planned).
    int64_t availableStart = 0, availableEnd = 0;
    for (const auto &source : available) {
        int64_t lo = source.time.minuteOldestMs;
        if (tfMs_ % kHourMs == 0 && source.time.hourThroughMs > source.time.hourOldestMs && source.time.hourOldestMs > 0)
            lo = std::min(lo, source.time.hourOldestMs);
        if (lo == INT64_MAX || lo >= source.time.endMs) continue;
        lo = std::max(lo, source.time.oldestMs);
        availableStart = availableEnd ? std::min(availableStart, lo) : lo;
        availableEnd = std::max(availableEnd, source.time.endMs);
    }
    if (availableStart != availableStartMs_ || availableEnd != availableEndMs_) {
        availableStartMs_ = availableStart;
        availableEndMs_ = availableEnd;
        dirty_ = true;
    }
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
        if (span.rank.tier != SpanTier::Visible && span.rank.tier != SpanTier::Fallback)
            for (auto &[name, source] : slot.sources) source.drawCompleteEnd.reset();
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
    // CPU estimates of sources admitted but neither built nor requested yet
    // (the cache pins only claims and running builds).
    size_t cpuAdmitted = 0;
    for (const auto &[id, slot] : slots_)
        for (const auto &planned : slot.plan.sources) {
            const auto it = slot.sources.find(planned.source);
            if (it == slot.sources.end() || (!it->second.ready && !it->second.pending))
                cpuAdmitted += estimate(id, planned.source, false);
        }
    stats_.suppressed = 0;
    cpuSuppressed_ = false;
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
        // A full node (no credit) still makes room for this span from strictly
        // lower-ranked content (recent-tf, farther prefetch): the node frees
        // what the snapshot stops listing before it uploads anything new, so
        // the victims' bytes count as credit at once (S5c: without this, stale
        // prefetch behind a pan kept the cap full and starved the new side).
        while (!guarded && !blocked && credit < need) {
            auto victim = slots_.end();
            for (auto it = slots_.begin(); it != slots_.end(); ++it) {
                const auto &rank = it->second.plan.rank;
                if (!isGuarded(rank.tier) && span.rank < rank && (victim == slots_.end() || victim->second.plan.rank < rank))
                    victim = it;
            }
            if (victim == slots_.end()) break;
            sLog_Probe("heatmap.controller.evict", "chart=" << chart_ << " tile=" << victim->first.tile << " tier="
                       << spanTierName(victim->second.plan.rank.tier) << " for=" << span.id.tile << " credit");
            const size_t bytesOf = gpuBytes(victim->second), freed = outstanding(victim->second);
            used -= std::min(used, bytesOf);
            credit += bytesOf;
            reserved -= std::min(freed, reserved);
            slots_.erase(victim); // releases its CPU claims
            ++stats_.evictions;
            dirty_ = true;
        }
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
            cpuSuppressed_ = cpuSuppressed_ || cpuOver();
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

    // What each source needs now (after admission, before the ceiling).
    std::map<SpanId, std::vector<SourceNeed>> needs;
    for (const auto &[id, slot] : slots_) {
        auto &list = needs[id];
        for (const auto &planned : slot.plan.sources) list.push_back(need(slot, planned));
    }

    // Process-wide CPU ceiling over wanted decoded chunks plus span images.
    // This chart commits what it pins; above the ceiling it gives up recent-tf,
    // then prefetch (far first), then fallback, then visible spans farthest from
    // the view centre. The nearest visible span always stays, even alone above
    // the ceiling, so a chart never shows nothing.
    // This chart's commitment: the unique chunk keys it keeps wanted, plus the
    // reservation of builds it still has to request (running jobs own their
    // own keys and reservations; claimed images are counted by the cache).
    std::vector<ChunkBytes> keys;
    size_t reservation = 0;
    auto commitment = [&] {
        keys.clear();
        reservation = stats_.liveBytes;
        std::unordered_set<ChunkKey, ChunkKeyHash> seen;
        for (const auto &[id, slot] : slots_) {
            const auto &list = needs.at(id);
            for (size_t i = 0; i < slot.plan.sources.size(); ++i) {
                const auto &planned = slot.plan.sources[i];
                const auto it = slot.sources.find(planned.source);
                const bool requested = it != slot.sources.end() && it->second.pending &&
                                       *it->second.pending == list[i].desired;
                if (list[i].build && !requested) reservation += estimate(id, planned.source, false);
                for (const auto &key : list[i].want)
                    if (seen.insert(key).second) {
                        const auto stored = store_.cached(key);
                        keys.push_back({key, stored ? stored->bytes : chunkCost(key), bool(stored)});
                    }
            }
        }
        for (const auto &key : liveWanted_) if (seen.insert(key).second) {
            const auto stored = store_.cached(key);
            keys.push_back({key, stored ? stored->bytes : chunkCost(key), bool(stored)});
        }
        return cache_.projectedCpuBytes(this, keys, reservation);
    };
    const size_t ceiling = cache_.cpuCeiling();
    size_t total = commitment();
    std::optional<SpanId> keeper; // the nearest visible span
    for (const auto &[id, slot] : slots_)
        if (slot.plan.rank.tier == SpanTier::Visible && (!keeper || slot.plan.rank < slots_.at(*keeper).plan.rank))
            keeper = id;
    auto lossOrder = [](SpanTier tier) {
        return tier == SpanTier::RecentTf ? 0 : tier == SpanTier::Prefetch ? 1 : tier == SpanTier::Fallback ? 2 : 3;
    };
    std::vector<SpanId> refusedSpans;
    size_t refusedBytes = 0;
    while (total > ceiling) {
        auto victim = slots_.end();
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            if (keeper && it->first == *keeper) continue;
            const auto &rank = it->second.plan.rank;
            if (victim == slots_.end()) { victim = it; continue; }
            const auto &worst = victim->second.plan.rank;
            if (std::pair(lossOrder(rank.tier), -rank.distance) < std::pair(lossOrder(worst.tier), -worst.distance))
                victim = it;
        }
        if (victim == slots_.end()) break;
        const auto id = victim->first;
        const auto tier = victim->second.plan.rank.tier;
        const size_t before = total;
        slots_.erase(victim); // releases its CPU claims; its running jobs keep their keys
        needs.erase(id);
        total = commitment();
        if (tier == SpanTier::Visible) {
            refusedSpans.push_back(id);
            refusedBytes += before - std::min(before, total);
        } else if (tier == SpanTier::Prefetch) {
            ++stats_.suppressed;
        }
        cpuSuppressed_ = true;
        dirty_ = true;
    }
    stats_.refused = refusedSpans.size();
    if (refusedSpans != cpuRefused_ || refusedBytes != cpuRefusedBytes_) dirty_ = true;
    cpuRefused_ = std::move(refusedSpans);
    cpuRefusedBytes_ = refusedBytes;
    if (!cpuRefused_.empty()) {
        stats_.refusals += cpuRefused_.size();
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - lastRefusalWarnMs_ >= 5000) {
            sLog_Warning("Heatmap CPU ceiling refused visible spans chart=" << chart_ << " symbol=" << symbol_
                         << " tf=" << tfMs_ << " spans=" << cpuRefused_.size() << " bytes=" << cpuRefusedBytes_
                         << " committed=" << total << " ceiling=" << ceiling
                         << " (suppressed " << quietRefusals_ << ")");
            lastRefusalWarnMs_ = now;
            quietRefusals_ = 0;
        } else {
            ++quietRefusals_;
        }
    }

    // Chunk demand: the best rank of any span needing a key wins. Built sources
    // keep only their open chunks; retained spans need nothing unless a GPU loss
    // makes them rebuild.
    std::unordered_map<ChunkKey, int, ChunkKeyHash> demands;
    for (const auto &[id, slot] : slots_)
        for (const auto &n : needs.at(id))
            for (const auto &key : n.want) {
                auto [it, inserted] = demands.emplace(key, slot.plan.rank.fetchPriority());
                if (!inserted) it->second = std::max(it->second, slot.plan.rank.fetchPriority());
            }
    for (const auto &key : liveWanted_) demands[key] = 1'000'000;
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
    // chunks are local (failed chunks are left out and draw loading). A changed
    // plan invalidates `expected` at once, so an older build in flight cannot
    // complete as current; the published version stays as a stale fallback.
    bool refused = false, visibleReady = true, hits = false;
    for (const auto &span : plan) {
        const auto it = slots_.find(span.id);
        if (it == slots_.end()) continue;
        auto &slot = it->second;
        auto &list = needs.at(span.id);
        for (size_t i = 0; i < slot.plan.sources.size(); ++i) {
            auto &n = list[i];
            auto &source = slot.sources[slot.plan.sources[i].source];
            const bool done = source.ready || (n.complete && n.input.chunks.empty());
            if (slot.plan.rank.tier == SpanTier::Visible && !done && !source.failed) visibleReady = false;
            // A changed desired key republishes the stale flag at once.
            const auto &desired = n.build || !source.ready ? n.desired : source.ready->key;
            if (source.expected != desired) {
                source.expected = desired;
                dirty_ = true;
            }
            if (!n.build) continue;
            if (!n.complete || n.input.chunks.empty()) continue;
            if (source.pending == n.input.key || source.failed == n.input.key) continue;
            if (auto hit = cache_.find(n.input.key)) {
                setReady(source, std::move(hit));
                hits = true; // demand above still wants its chunks: reconcile again
                continue;
            }
            if (refused) continue;
            const auto key = n.input.key;
            const auto serial = serial_;
            if (cache_.request(std::move(n.input), slot.plan.rank.fetchPriority(), estimate(span.id, key.source, false),
                               this, [this, serial, key](SpanSourceBuildPtr build, const QString &error) {
                                   onBuilt(serial, key, std::move(build), error);
                               }))
                source.pending = key;
            else refused = true;
        }
    }
    refused_ = refused;
    if (hits) schedule();
    // Commit after the builds: requests made above are owned by their jobs now.
    commitment();
    stats_.committedBytes = reservation;
    for (const auto &in : keys) stats_.committedBytes += in.bytes;
    const bool atKeeper = slots_.size() <= 1;
    cache_.commitCpu(this, std::move(keys), reservation, atKeeper);
    if (cache_.committedCpuBytes() > ceiling && !atKeeper) schedule(); // shed what this pass added
    if (!visibleReady_ && visibleReady) {
        // The new tf's view is complete: fallback spans become the recent-tf tier.
        visibleReady_ = true;
        dirty_ = true;
        if (!retained_.empty()) schedule();
    }
    if (dirty_) publish();
    refreshLive();
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
    set->refused = cpuRefused_;
    set->refusedBytes = cpuRefusedBytes_;
    set->availableStartMs = availableStartMs_;
    set->availableEndMs = availableEndMs_;
    for (const auto &[id, slot] : slots_) {
        SpanSnapshot span{id, slot.plan.rank, {}, true};
        for (const auto &planned : slot.plan.sources) {
            const auto it = slot.sources.find(planned.source);
            SpanSourceSnapshot source{planned.source, {}, false};
            if (it != slot.sources.end()) {
                source.build = it->second.ready;
                source.stale = source.build && it->second.expected && source.build->key != *it->second.expected;
                source.failed = it->second.failed && !it->second.pending;
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
        mergeLatestResolution();
    }
    ++stats_.publications;
    dirty_ = false;
    emit snapshotChanged();
}
} // namespace heatmap
