#include "protocol/SentinelStreamClient.hpp" // shared-frame metatype
#include "render/heatmap/HeatmapSourceController.hpp"
#include "render/heatmap/HeatmapCellQuery.hpp"
#include "../servermodel/FakeChunkTransport.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QTimer>
#include <gtest/gtest.h>
#include <climits>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <map>
#include <set>

// Fake-node tests for slice S5b: FakeChunkTransport, a manual build executor
// (jobs run only when the test says), no GPU and no sleeps.
namespace {
using namespace heatmap;
// Aligned to 5m tiles (320 minutes), so 1m and 5m tile edges coincide.
constexpr int64_t epoch = ((recording::kHmc2MinMs / (320 * kMinuteMs)) + 3) * (320 * kMinuteMs);
constexpr int64_t availableStart = epoch - 12 * kHourMs, availableEnd = epoch + 36 * kHourMs;
constexpr int64_t tileMs = tiles::kTileColumns * kMinuteMs;
// Deliberately not "near"/"deep" by position: the controller must not care.
bool coarse(const std::string &source) { return findChunkSource(source)->hourLevel; }

// Hour level (oldest, committed-through) for sources that have one; 0 = none.
ChunkAvailability availability(int64_t end = availableEnd, int64_t hourFrom = 0, int64_t hourThrough = 0) {
    ChunkAvailability a;
    a.symbol = "BTC-USD";
    a.chunkWireVersion = kChunkWireVersion;
    for (const auto &source : kChunkSources) {
        protocol::chunkwire::SourceInfo s;
        s.id = source.id;
        s.latestGrid = protocol::chunkwire::GridInfo{};
        s.latestGrid->priceScale = 100;
        s.levels.push_back({kMinuteMs, kHourMs, end, availableStart, end - kMinuteMs});
        if (source.hourLevel && hourThrough)
            s.levels.push_back({kHourMs, kDayMs, hourThrough, hourFrom, hourThrough - kHourMs});
        a.sources.push_back(std::move(s));
    }
    return a;
}
// Hour rollups exist from here on (hour-level chunks omit earlier columns but
// still scan their whole day, as the server does).
int64_t hourOldest = 0;
// Fine source: $1 rows over $95-$105. Coarse source: $5 rows over $0-$200.
ChunkFramePtr body(const ChunkKey &key, uint64_t revision = 1) {
    auto frame = std::make_shared<ChunkFrame>();
    frame->key = key;
    const int64_t span = chunkSpanMs(key.source, key.levelMs);
    frame->state = {true, key.startMs + span, revision};
    frame->contentHash = revision;
    const bool isCoarse = coarse(key.source);
    const int64_t tick = isCoarse ? 500 : 100, lo = isCoarse ? 0 : 9500, hi = isCoarse ? 20000 : 10500;
    frame->columns = {key.symbol, std::string(findChunkSource(key.source)->hmc2Layer), key.levelMs, key.startMs,
                      key.startMs + span, {}, {}};
    frame->columns.scannedRanges = {{key.startMs, key.startMs + span}};
    for (auto t = key.startMs; t < key.startMs + span; t += key.levelMs) {
        if (key.levelMs == kHourMs && t < hourOldest) continue;
        NativeColumn n;
        n.grid = {1, tick, 100};
        n.observedMs = uint64_t(key.levelMs);
        n.baseRow = lo / tick;
        n.coverage[0] = {{lo / tick, hi / tick - 1, uint64_t(key.levelMs)}};
        n.coverage[1] = n.coverage[0];
        n.entries = {{0, recording::encodeSize(double(1 + revision))}};
        frame->columns.columns.push_back({t, uint64_t(key.levelMs), 0, {std::move(n)}});
    }
    return frame;
}
void revise(ChunkStore &store, const ChunkKey &key, uint64_t revision) {
    const auto frame = body(key, revision);
    ASSERT_TRUE(store.put(key, std::shared_ptr<const SparseColumns>(frame, &frame->columns), frame->state,
                          frame->contentHash));
}
void drain() {
    for (int i = 0; i < 32; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
std::set<const SpanSourceBuild *> builds(const SpanSet &set, std::optional<SpanTier> tier = {}) {
    std::set<const SpanSourceBuild *> out;
    for (const auto &span : set.spans)
        if (!tier || span.rank.tier == *tier)
            for (const auto &source : span.sources)
                if (source.build) out.insert(source.build.get());
    return out;
}
bool visibleComplete(const SpanSet &set) {
    bool any = false;
    for (const auto &span : set.spans)
        if (span.rank.tier == SpanTier::Visible) {
            any = true;
            if (!span.complete) return false;
        }
    return any;
}

class SourceController : public testing::Test {
protected:
    int argc = 1;
    char name[16] = "controller";
    char *argv[2]{name, nullptr};
    QCoreApplication app{argc, argv};
    ChunkStore store;
    FakeChunkTransport transport;
    ChunkFetcher fetcher{store, transport};
    std::deque<std::function<void()>> jobs;
    std::unique_ptr<SpanSourceCache> cache;
    std::vector<std::unique_ptr<HeatmapSourceController>> charts;
    size_t answered = 0;
    std::function<void()> beforeBuild, buildCheckpoint;

    void SetUp() override {
        hourOldest = 0;
        makeCache();
        transport.goOnline();
        transport.push(availability());
        drain();
    }
    void TearDown() override {
        charts.clear();
        jobs.clear();
        cache.reset();
        drain();
    }
    size_t visibleUploadBytes(const SpanSet &set) {
        size_t bytes = 0;
        for (const auto &span : set.spans)
            if (span.rank.tier == SpanTier::Visible)
                for (const auto &source : span.sources) bytes += source.build ? source.build->uploadBytes : 0;
        return bytes;
    }
    void makeCache(size_t maxBytes = 256ull << 20, size_t maxJobs = 8) {
        SpanSourceCache::Options options;
        options.maxBytes = maxBytes;
        options.maxJobs = maxJobs;
        options.beforeBuild = [this] { if (beforeBuild) beforeBuild(); };
        options.buildCheckpoint = [this] { if (buildCheckpoint) buildCheckpoint(); };
        options.executor = [this](std::function<void()> job, int) { jobs.push_back(std::move(job)); };
        cache = std::make_unique<SpanSourceCache>(options);
    }
    SpanSourceInput input() {
        const ChunkKey key{"BTC-USD", std::string(kChunkSources[0].id), kMinuteMs, epoch / kHourMs * kHourMs};
        revise(store, key, 1);
        auto chunk = store.cached(key);
        SpanSourceInput out;
        out.key = {{key.symbol, kMinuteMs, tiles::tileOfBucket(epoch / kMinuteMs)}, key.source,
                   availableStart, availableEnd, 100, 0,
                   {{key.symbol, key.source, key.levelMs, key.startMs, chunk->generation, chunk->sealed}}};
        out.chunks.push_back(std::move(chunk));
        return out;
    }
    void runFirst() {
        ASSERT_FALSE(jobs.empty());
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
    }
    HeatmapSourceController &chart(size_t gpuBytes = 320ull << 20, size_t estimate = 64 * 1024) {
        HeatmapSourceController::Options options;
        options.gpuBytes = gpuBytes;
        options.sourceEstimateBytes = estimate;
        options.capacityPollMs = 0;
        charts.push_back(std::make_unique<HeatmapSourceController>(store, fetcher, *cache, options));
        return *charts.back();
    }
    void view(HeatmapSourceController &c, int64_t tfMs = kMinuteMs) {
        c.setView("BTC-USD", tfMs, double(epoch), double(epoch + tileMs));
    }
    // Answers every outstanding request (and follow-ups); builds stay queued.
    void answerAll() {
        for (int i = 0; i < 1000; ++i) {
            drain();
            if (answered == transport.requests.size()) break;
            while (answered < transport.requests.size()) {
                const auto request = transport.requests[answered++];
                for (size_t k = 0; k < request.starts.size(); ++k) transport.reply(request.id, body(request.key(k)));
            }
        }
    }
    // Fake node frame: uploads every image of the latest snapshot and reports.
    size_t upload(HeatmapSourceController &c, size_t freeBytes) {
        std::vector<SpanSourceKey> uploaded;
        for (const auto &span : c.latestSnapshot()->spans)
            for (const auto &source : span.sources)
                if (source.build && source.build->gpu) uploaded.push_back(source.build->key);
        const size_t count = uploaded.size();
        c.capacity()->report(freeBytes, std::move(uploaded));
        c.pollCapacity();
        step();
        return count;
    }
    // One round: every outstanding request answered, then queued builds run.
    bool step() {
        drain();
        bool progressed = false;
        while (answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t i = 0; i < request.starts.size(); ++i) transport.reply(request.id, body(request.key(i)));
            progressed = true;
        }
        drain();
        while (!jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            job();
            progressed = true;
        }
        drain();
        return progressed;
    }
    void settle() {
        for (int i = 0; i < 200 && step(); ++i) {}
        ASSERT_TRUE(jobs.empty());
        ASSERT_EQ(answered, transport.requests.size());
    }
    std::vector<ChunkKey> requestedKeys() const {
        std::vector<ChunkKey> out;
        for (const auto &r : transport.requests)
            for (size_t i = 0; i < r.starts.size(); ++i) out.push_back(r.key(i));
        return out;
    }
};

TEST_F(SourceController, ChunkSetAndPrefetchOrderCoverBothSourcesAndTwoTilesPerSide) {
    auto &a = chart();
    view(a);
    // Expected best fetch priority per key, from the planner.
    std::vector<SourceAvailability> sources;
    for (const auto &s : kChunkSources)
        sources.push_back({std::string(s.id), {availableStart, availableEnd, 0}});
    const auto plan = planSpans("BTC-USD", kMinuteMs, double(epoch), double(epoch + tileMs), sources);
    std::map<std::tuple<std::string, int64_t>, int> priority;
    std::set<int64_t> planned;
    for (const auto &span : plan) {
        planned.insert(span.id.tile);
        ASSERT_EQ(span.sources.size(), kChunkSources.size());
        for (const auto &source : span.sources)
            for (const auto &key : source.chunks) {
                auto &p = priority[{key.source, key.startMs}];
                p = std::max(p, span.rank.fetchPriority());
            }
    }
    const int64_t visible = tiles::tileOfBucket(epoch / kMinuteMs);
    EXPECT_EQ(planned, (std::set<int64_t>{visible - 2, visible - 1, visible, visible + 1, visible + 2}));
    // Rounds of admitted requests are in rank order: nothing in a later round
    // outranks anything in an earlier round, and the first round is the view.
    std::vector<std::pair<int, int>> rounds; // min, max priority per round
    std::set<std::string> firstRoundSources;
    for (int round = 0; round < 100; ++round) {
        drain();
        if (answered == transport.requests.size()) break;
        int lo = INT_MAX, hi = INT_MIN;
        for (size_t r = answered; r < transport.requests.size(); ++r)
            for (size_t i = 0; i < transport.requests[r].starts.size(); ++i) {
                const auto key = transport.requests[r].key(i);
                const int p = priority.at({key.source, key.startMs});
                lo = std::min(lo, p);
                hi = std::max(hi, p);
                if (rounds.empty()) firstRoundSources.insert(key.source);
            }
        rounds.push_back({lo, hi});
        step();
    }
    ASSERT_GE(rounds.size(), 2u);
    EXPECT_EQ(rounds.front().first, (SpanRank{SpanTier::Visible, 0}).fetchPriority());
    EXPECT_EQ(firstRoundSources.size(), kChunkSources.size());
    for (size_t i = 1; i < rounds.size(); ++i) EXPECT_GE(rounds[i - 1].first, rounds[i].second) << i;
    // Every planned key was requested exactly once.
    EXPECT_EQ(requestedKeys().size(), priority.size());
    settle();
    const auto snapshot = a.latestSnapshot();
    ASSERT_EQ(snapshot->spans.size(), 5u);
    for (const auto &span : snapshot->spans) {
        EXPECT_TRUE(span.complete);
        ASSERT_EQ(span.sources.size(), 2u);
        // Coarsest common tick first (fill order), whatever the source ids are.
        EXPECT_TRUE(coarse(span.sources[0].source));
        EXPECT_GT(span.sources[0].build->commonUnits, span.sources[1].build->commonUnits);
    }
}

TEST_F(SourceController, TwoChartsShareEveryChunkAndEverySpanBuild) {
    auto &a = chart();
    auto &b = chart();
    view(a);
    view(b);
    settle();
    const auto sa = a.latestSnapshot(), sb = b.latestSnapshot();
    ASSERT_EQ(sa->spans.size(), 5u);
    ASSERT_EQ(sb->spans.size(), 5u);
    std::set<std::tuple<std::string, int64_t, int64_t>> unique;
    for (const auto &key : requestedKeys()) unique.emplace(key.source, key.levelMs, key.startMs);
    EXPECT_EQ(requestedKeys().size(), unique.size());
    EXPECT_EQ(store.stats().loads, unique.size());
    EXPECT_LE(store.stats().bytes, store.stats().maxBytes);
    // One build per (span, source), and both charts publish the same objects.
    EXPECT_EQ(cache->stats().builds, 10u);
    EXPECT_EQ(builds(*sa), builds(*sb));
    EXPECT_EQ(builds(*sa).size(), 10u);
    // A tick change never rebuilds (sources are tick-free).
    a.setTickRequest(TickMode::Manual, 100);
    step();
    EXPECT_EQ(cache->stats().builds, 10u);
}

TEST_F(SourceController, RecentTimeframeReturnsWithoutRequestsOrBuilds) {
    auto &a = chart();
    view(a, kMinuteMs);
    settle();
    const auto oneMinute = builds(*a.latestSnapshot(), SpanTier::Visible);
    ASSERT_EQ(oneMinute.size(), 2u);
    view(a, 5 * kMinuteMs);
    settle();
    EXPECT_EQ(a.latestSnapshot()->tfMs, 5 * kMinuteMs);
    EXPECT_EQ(builds(*a.latestSnapshot(), SpanTier::RecentTf), oneMinute);
    // No CPU room beyond what the slots claim: the LRU keeps nothing else, so
    // only recent-tf retention can keep the 1m view.
    cache->setMaxBytes(cache->stats().claimedBytes);
    const auto requests = requestedKeys().size();
    // Switch back: the visible spans are published on the first reconcile,
    // before any build job runs or any chunk is answered.
    view(a, kMinuteMs);
    drain();
    EXPECT_EQ(a.latestSnapshot()->tfMs, kMinuteMs);
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
    EXPECT_EQ(builds(*a.latestSnapshot(), SpanTier::Visible), oneMinute);
    EXPECT_EQ(requestedKeys().size(), requests);
    settle();
    EXPECT_EQ(requestedKeys().size(), requests); // zero requests overall
}

TEST_F(SourceController, RevisionRebuildsBothChartsAffectedSpansExactlyOnce) {
    auto &a = chart();
    auto &b = chart();
    view(a);
    view(b);
    settle();
    const auto before = a.latestSnapshot();
    // A coarse chunk the visible span depends on.
    const SpanSnapshot *visible = nullptr;
    for (const auto &span : before->spans)
        if (span.rank.tier == SpanTier::Visible) visible = &span;
    ASSERT_TRUE(visible);
    const auto dependency = visible->sources[0].build->key.generations.front();
    const ChunkKey key{dependency.symbol, dependency.source, dependency.levelMs, dependency.startMs};
    std::set<const SpanSourceBuild *> affected;
    for (const auto *build : builds(*before)) {
        const auto &g = build->key.generations;
        if (std::find(g.begin(), g.end(), dependency) != g.end()) affected.insert(build);
    }
    ASSERT_GE(affected.size(), 1u);
    const auto built = cache->stats().builds;
    const auto requests = requestedKeys().size();
    const auto revised = body(key, 2);
    ASSERT_TRUE(store.put(key, std::shared_ptr<const SparseColumns>(revised, &revised->columns), revised->state,
                          revised->contentHash));
    settle();
    EXPECT_EQ(cache->stats().builds, built + affected.size());
    EXPECT_EQ(requestedKeys().size(), requests);
    const auto sa = a.latestSnapshot(), sb = b.latestSnapshot();
    EXPECT_EQ(builds(*sa), builds(*sb));
    size_t replaced = 0;
    for (const auto &span : sa->spans) {
        EXPECT_TRUE(span.complete);
        for (const auto &source : span.sources) {
            const auto old = std::find_if(before->spans.begin(), before->spans.end(),
                                          [&](const auto &s) { return s.id == span.id; });
            ASSERT_NE(old, before->spans.end());
            const auto previous = std::find_if(old->sources.begin(), old->sources.end(),
                                               [&](const auto &s) { return s.source == source.source; });
            const bool touched = affected.contains(previous->build.get());
            EXPECT_EQ(source.build != previous->build, touched);
            replaced += touched;
        }
    }
    EXPECT_EQ(replaced, affected.size());
}

TEST_F(SourceController, SuppressedPrefetchReturnsAfterCapacityEpochAndStaticViewSettles) {
    constexpr size_t estimate = 64 * 1024, spanBytes = 2 * estimate, need = spanBytes + (spanBytes + 9) / 10;
    auto &a = chart(64ull << 20, estimate);
    // The fake node has no free bytes: only the visible span is admitted.
    a.capacity()->reportFree(0);
    a.pollCapacity();
    view(a);
    settle();
    EXPECT_EQ(a.stats().suppressed, 4u);
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 1u);
    EXPECT_EQ(cache->stats().builds, 2u);
    // The node uploads the view; free bytes below bytes + 10% admit nothing.
    EXPECT_EQ(upload(a, need - 1), 2u);
    settle();
    EXPECT_EQ(a.latestSnapshot()->spans.size(), 1u);
    EXPECT_EQ(cache->stats().builds, 2u);
    // A release leaves exactly bytes + 10%: only the capacity epoch changed,
    // and the nearest prefetch span returns, strictly by rank.
    a.capacity()->reportFree(need);
    a.pollCapacity();
    settle();
    const auto admitted = a.latestSnapshot();
    ASSERT_EQ(admitted->spans.size(), 2u);
    EXPECT_EQ(admitted->spans[1].rank, (SpanRank{SpanTier::Prefetch, 1}));
    EXPECT_TRUE(admitted->spans[1].complete);
    EXPECT_EQ(cache->stats().builds, 4u);
    EXPECT_EQ(a.stats().suppressed, 3u);
    size_t prefetchBytes = 0;
    for (const auto &source : admitted->spans[1].sources) prefetchBytes += source.build->uploadBytes;
    // Static view: the node uploads that span, then keeps reporting without new room.
    upload(a, need - prefetchBytes);
    const auto stable = cache->stats();
    const auto stats = a.stats();
    const auto version = a.latestSnapshot()->version;
    for (int i = 0; i < 20; ++i) {
        a.capacity()->reportFree(need - prefetchBytes);
        a.pollCapacity();
        settle();
    }
    EXPECT_EQ(cache->stats().builds, stable.builds);
    EXPECT_EQ(cache->stats().evictions, stable.evictions);
    EXPECT_EQ(a.stats().evictions, 0u);
    EXPECT_EQ(a.stats().admissions, stats.admissions);
    EXPECT_EQ(a.latestSnapshot()->version, version);
}

TEST_F(SourceController, TightGpuBudgetEvictsOnlyLowerRanks) {
    constexpr size_t estimate = 64 * 1024;
    // Room for the visible span plus one prefetch span by estimate.
    auto &a = chart(2 * 2 * estimate + 2 * estimate / 10 + 1, estimate);
    view(a);
    drain();
    const auto first = a.latestSnapshot();
    ASSERT_EQ(first->spans.size(), 2u);
    EXPECT_EQ(first->spans[1].rank, (SpanRank{SpanTier::Prefetch, 1}));
    // Pan one tile right: the old visible tile is now prefetch distance 1 and
    // the admitted set follows rank; nothing evicts the new visible span.
    a.setView("BTC-USD", kMinuteMs, double(epoch + tileMs), double(epoch + 2 * tileMs));
    drain();
    const auto panned = a.latestSnapshot();
    ASSERT_GE(panned->spans.size(), 1u);
    EXPECT_EQ(panned->spans[0].rank.tier, SpanTier::Visible);
    EXPECT_EQ(panned->spans[0].id.tile, first->spans[0].id.tile + 1);
    for (size_t i = 1; i < panned->spans.size(); ++i) EXPECT_EQ(panned->spans[i].rank.distance, 1);
}

TEST_F(SourceController, AutoCrossesTheFineBandEdgeAndManualReportsTheVeil) {
    auto &a = chart();
    view(a);
    settle();
    const auto snapshot = a.latestSnapshot();
    const auto &summary = snapshot->resolution;
    EXPECT_EQ(summary.tfMs, kMinuteMs);
    EXPECT_EQ(summary.columns.size(), 5u * tiles::kTileColumns);
    const double lo = double(epoch), hi = double(epoch + tileMs);
    // Rows in view inside the fine band: $1. Beyond it: the coarse $5.
    EXPECT_EQ(autoTickUnits(summary, 0, lo, hi, 96, 104, 100), 100);
    EXPECT_EQ(autoTickUnits(summary, 100, lo, hi, 94, 104, 100), 500);
    EXPECT_EQ(autoTickUnits(summary, 500, lo, hi, 96, 104, 100), 100);
    // Manual offers $1 (some loaded data builds it) and reports where it veils.
    const auto presets = manualPresetUnits(summary, 1000);
    EXPECT_EQ(presets, (std::vector<int64_t>{100, 200, 500, 1000}));
    const auto veil = veiledRanges(summary, 100, lo, hi, 94, 106);
    ASSERT_EQ(veil.size(), 2u);
    EXPECT_EQ(veil[0], (VeiledRange{epoch, epoch + tileMs, {9400, 9500}}));
    EXPECT_EQ(veil[1], (VeiledRange{epoch, epoch + tileMs, {10500, 10600}}));
    EXPECT_TRUE(veiledRanges(summary, 500, lo, hi, 94, 106).empty());
}

// C1: two nested executor invocations represent occupied worker slots; the
// other six admissions are queued. No timers, sleeps or scheduling races.
TEST_F(SourceController, TimeframeSwitchCancelsQueuedJobsBehindTwoRunningBuilds) {
    auto &a = chart();
    view(a);
    answerAll();
    ASSERT_EQ(cache->stats().jobs, 8u);
    ASSERT_EQ(jobs.size(), 8u);
    int started = 0;
    beforeBuild = [&] {
        ++started;
        if (started == 1) {
            runFirst(); // second occupied worker
        } else if (started == 2) {
            view(a, 5 * kMinuteMs);
            EXPECT_EQ(cache->stats().jobs, 2u); // six queued A jobs dropped immediately
            answerAll(); // B can enter before either cancelled worker acknowledges
            EXPECT_GT(cache->stats().jobs, 2u);
        }
    };
    runFirst();
    EXPECT_EQ(started, 2);
    drain();
    for (int i = 0; i < 6; ++i) runFirst(); // stale executor closures never enter the builder
    EXPECT_EQ(started, 2);
    runFirst(); // first B build
    EXPECT_EQ(started, 3);
    beforeBuild = {};
    settle();
    EXPECT_EQ(a.stats().staleResults, 0u);
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

// C2: stop after composition and after image allocation. Actual chunk bodies,
// reservations, image bytes and admission must all be released, exactly once.
TEST_F(SourceController, RunningCancellationStopsAtCheckpointAndReleasesResources) {
    for (int stopAt : {2, 4}) {
        SCOPED_TRACE(stopAt);
        QObject consumer;
        auto in = input();
        const auto key = in.key;
        std::weak_ptr<const StoredChunk> chunk = in.chunks.front();
        const size_t baseline = cache->committedCpuBytes();
        const size_t chunkBytes = in.chunks.front()->bytes;
        int checkpoints = 0, completions = 0, freed = 0;
        const auto connection = QObject::connect(cache.get(), &SpanSourceCache::capacityFreed, &consumer, [&] { ++freed; });
        buildCheckpoint = [&] {
            if (++checkpoints != stopAt) return;
            cache->cancelRequests(&consumer);
            EXPECT_EQ(cache->stats().jobs, 1u);
            EXPECT_EQ(cache->stats().reservedBytes, 1234u);
            EXPECT_EQ(cache->committedCpuBytes(), baseline + chunkBytes + 1234);
            EXPECT_FALSE(chunk.expired());
        };
        ASSERT_TRUE(cache->request(std::move(in), 0, 1234, &consumer,
                                  [&](auto, auto) { ++completions; }));
        store.clear(); // only the job owns the StoredChunk now
        runFirst();
        EXPECT_EQ(checkpoints, stopAt); // the build did not proceed to the next phase
        EXPECT_TRUE(chunk.expired());
        EXPECT_EQ(cache->stats().jobs, 1u); // no early ledger release
        drain();
        EXPECT_EQ(cache->stats().jobs, 0u);
        EXPECT_EQ(cache->stats().reservedBytes, 0u);
        EXPECT_EQ(cache->committedCpuBytes(), baseline);
        EXPECT_EQ(cache->stats().liveBytes, 0);
        EXPECT_EQ(cache->stats().failures, 0u); // cancellation is not a build failure
        EXPECT_EQ(completions, 0);
        EXPECT_EQ(freed, 1);
        EXPECT_FALSE(cache->find(key));
        QObject::disconnect(connection);
        buildCheckpoint = {};
    }
}

// C3: cancellation is per consumer, including while a shared job is running.
TEST_F(SourceController, SharedRunningBuildSurvivesOneConsumersCancellation) {
    QObject a, b;
    auto in = input();
    const auto key = in.key;
    int calledA = 0, calledB = 0;
    ASSERT_TRUE(cache->request(in, 0, 100, &a, [&](auto, auto) { ++calledA; }));
    ASSERT_TRUE(cache->request(in, 0, 100, &b, [&](auto build, auto error) {
        ++calledB;
        EXPECT_TRUE(build);
        EXPECT_TRUE(error.isEmpty());
    }));
    beforeBuild = [&] { cache->cancelRequests(&a); };
    runFirst();
    beforeBuild = {};
    drain();
    EXPECT_EQ(calledA, 0);
    EXPECT_EQ(calledB, 1);
    EXPECT_TRUE(cache->find(key));
    EXPECT_EQ(cache->stats().builds, 1u);
}

TEST_F(SourceController, SharedChartBuildsSurviveOtherChartsTimeframeSwitch) {
    auto &a = chart();
    auto &b = chart();
    view(a);
    view(b);
    answerAll();
    const auto admitted = cache->stats().builds;
    ASSERT_EQ(cache->stats().jobs, 8u);
    view(a, 5 * kMinuteMs);
    EXPECT_EQ(cache->stats().jobs, 8u); // every A key still has B's waiter
    for (size_t i = 0; i < admitted; ++i) runFirst();
    drain();
    EXPECT_TRUE(visibleComplete(*b.latestSnapshot()));
    EXPECT_EQ(a.stats().staleResults, 0u);
    settle();
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

// C4: cancelling old work does not release the already-built picture.
TEST_F(SourceController, TimeframeSwitchKeepsBuiltFallbackWhileNewBuildsWait) {
    auto &a = chart();
    view(a);
    settle();
    const auto picture = builds(*a.latestSnapshot(), SpanTier::Visible);
    ASSERT_FALSE(picture.empty());
    // Queue a revision of that picture, then leave while it is pending.
    const auto g = (*picture.begin())->key.generations.front();
    revise(store, {g.symbol, g.source, g.levelMs, g.startMs}, 2);
    drain();
    ASSERT_FALSE(jobs.empty());
    view(a, 5 * kMinuteMs);
    answerAll();
    EXPECT_EQ(builds(*a.latestSnapshot(), SpanTier::Fallback), picture);
    EXPECT_FALSE(visibleComplete(*a.latestSnapshot()));
    settle();
    EXPECT_EQ(builds(*a.latestSnapshot(), SpanTier::RecentTf), picture);
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

TEST_F(SourceController, ReturningTimeframeKeepsPendingFallbackRebuild) {
    auto &a = chart();
    view(a);
    settle();
    upload(a, 1ull << 30);
    view(a, 5 * kMinuteMs);
    drain(); // 5m chunks remain unanswered
    a.capacity()->report(1ull << 30, {}, true);
    a.pollCapacity();
    drain(); // GPU loss queued two 1m fallback source rebuilds
    const auto pending = cache->stats().jobs;
    ASSERT_EQ(pending, 2u);
    const auto admitted = cache->stats().builds;
    view(a, kMinuteMs);
    EXPECT_EQ(cache->stats().jobs, pending); // same keys are now visible again
    // Deliver before the new view's reconcile as well as across its serial.
    for (size_t i = 0; i < pending; ++i) runFirst();
    drain();
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
    EXPECT_EQ(a.stats().staleResults, 0u);
    settle();
    // The other four spans each need two fresh sources after loss.
    EXPECT_EQ(cache->stats().builds, admitted + 8);
}

// C5: queued, already-posted and reentrant completion removal. A replacement
// for the same key must never be erased by its cancelled predecessor.
TEST_F(SourceController, CancelledCompletionCannotConsumeSameKeyReplacement) {
    QObject consumer;
    auto in = input();
    int oldCalls = 0, newCalls = 0;
    ASSERT_TRUE(cache->request(in, 0, 100, &consumer, [&](auto, auto) { ++oldCalls; }));
    runFirst(); // completed on worker; owner-thread delivery still pending
    cache->cancelRequests(&consumer);
    ASSERT_TRUE(cache->request(in, 0, 100, &consumer, [&](auto build, auto) {
        ++newCalls;
        EXPECT_TRUE(build);
    }));
    drain(); // old completion must leave the new pending job alone
    EXPECT_EQ(cache->stats().jobs, 1u);
    runFirst();
    drain();
    EXPECT_EQ(oldCalls, 0);
    EXPECT_EQ(newCalls, 1);
    EXPECT_EQ(cache->stats().jobs, 0u);
}

TEST_F(SourceController, CompletionCanRemoveAnotherWaiterReentrantly) {
    QObject a, b;
    auto in = input();
    int calledA = 0, calledB = 0;
    ASSERT_TRUE(cache->request(in, 0, 100, &a, [&](auto, auto) {
        ++calledA;
        cache->cancelRequests(&b);
    }));
    ASSERT_TRUE(cache->request(in, 0, 100, &b, [&](auto, auto) { ++calledB; }));
    runFirst();
    drain();
    EXPECT_EQ(calledA, 1);
    EXPECT_EQ(calledB, 0);
    EXPECT_EQ(cache->stats().reservedBytes, 0u);
}

TEST_F(SourceController, CancelledQueuedClosuresOutliveControllerAndCache) {
    int started = 0;
    beforeBuild = [&] { ++started; };
    view(chart());
    answerAll();
    ASSERT_FALSE(jobs.empty());
    charts.clear();
    EXPECT_EQ(cache->stats().jobs, 0u);
    EXPECT_EQ(cache->committedCpuBytes(), 0u);
    cache.reset();
    while (!jobs.empty()) runFirst(); // no dereference of the captured, destroyed cache
    drain();
    EXPECT_EQ(started, 0);
    beforeBuild = {};
}

TEST_F(SourceController, PanKeepsStillNeededPendingKeys) {
    auto &a = chart();
    view(a);
    answerAll();
    const auto admitted = cache->stats().builds;
    // Same tile, different view bounds: all pending keys are still needed.
    a.setView("BTC-USD", kMinuteMs, double(epoch + kMinuteMs), double(epoch + tileMs - kMinuteMs));
    drain();
    EXPECT_EQ(cache->stats().builds, admitted);
    EXPECT_EQ(cache->stats().jobs, admitted);
    settle();
    EXPECT_EQ(cache->stats().builds, 10u); // five spans, two sources; none rebuilt
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

TEST_F(SourceController, OlderSerialBuildWaitersAreRemoved) {
    auto &a = chart();
    view(a, kMinuteMs);
    // Chunks arrive; the 1m builds are queued but not run.
    for (int i = 0; i < 20; ++i) {
        drain();
        if (answered == transport.requests.size()) break;
        while (answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t k = 0; k < request.starts.size(); ++k) transport.reply(request.id, body(request.key(k)));
        }
    }
    ASSERT_FALSE(jobs.empty());
    view(a, 5 * kMinuteMs); // new serial
    drain();
    const auto serial = a.latestSnapshot()->serial;
    settle();
    EXPECT_EQ(a.stats().staleResults, 0u);
    const auto snapshot = a.latestSnapshot();
    EXPECT_EQ(snapshot->serial, serial);
    EXPECT_EQ(snapshot->tfMs, 5 * kMinuteMs);
    for (const auto &span : snapshot->spans) EXPECT_EQ(span.id.tfMs, 5 * kMinuteMs);
    EXPECT_TRUE(visibleComplete(*snapshot));
}

TEST_F(SourceController, BuildSupersededByARevisionIsDroppedForTheNewerOne) {
    makeCache(256ull << 20, 64); // room to queue the rebuild next to the first build
    auto &a = chart();
    view(a);
    // Answer every chunk; keep the builds queued.
    for (int i = 0; i < 20; ++i) {
        drain();
        if (answered == transport.requests.size()) break;
        while (answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t k = 0; k < request.starts.size(); ++k) transport.reply(request.id, body(request.key(k)));
        }
    }
    ASSERT_FALSE(jobs.empty());
    const auto serial = a.latestSnapshot() ? a.latestSnapshot()->serial : 0;
    // Revise a chunk of the visible span while its first build is queued.
    const auto visible = tiles::tileOfBucket(epoch / kMinuteMs);
    const ChunkKey key{"BTC-USD", std::string(kChunkSources[0].id), kMinuteMs, epoch / kHourMs * kHourMs};
    const auto revised = body(key, 2);
    ASSERT_TRUE(store.put(key, std::shared_ptr<const SparseColumns>(revised, &revised->columns), revised->state,
                          revised->contentHash));
    settle();
    EXPECT_EQ(a.stats().staleResults, 0u);
    const auto snapshot = a.latestSnapshot();
    EXPECT_EQ(snapshot->serial, serial);
    const auto generation = store.generationOf(key);
    bool checked = false;
    for (const auto &span : snapshot->spans) {
        EXPECT_TRUE(span.complete);
        if (span.id.tile != visible) continue;
        for (const auto &source : span.sources)
            for (const auto &g : source.build->key.generations)
                if (g.source == key.source && g.startMs == key.startMs) {
                    EXPECT_EQ(g.generation, generation);
                    checked = true;
                }
    }
    EXPECT_TRUE(checked);
}

TEST_F(SourceController, BudgetsAreConfigurableAndATinyCpuTierKeepsOnlyTheView) {
    HeatmapBudgets budgets;
    budgets.cpuCeiling = 1;
    EXPECT_FALSE(HeatmapSourceController::applyBudgets(budgets, store, *cache));
    budgets.cpuCeiling = 1ull << 30;
    budgets.spanSources = 1;
    EXPECT_TRUE(HeatmapSourceController::applyBudgets(budgets, store, *cache));
    EXPECT_EQ(store.stats().maxBytes, budgets.decodedChunks);
    EXPECT_EQ(cache->maxBytes(), 1u);
    auto &a = chart();
    auto &b = chart();
    view(a);
    view(b);
    settle();
    // Visible spans always enter; prefetch has no CPU room. One shared build.
    EXPECT_EQ(cache->stats().builds, 2u);
    EXPECT_EQ(a.latestSnapshot()->spans.size(), 1u);
    EXPECT_EQ(builds(*a.latestSnapshot()), builds(*b.latestSnapshot()));
    size_t claimed = 0;
    for (const auto *build : builds(*a.latestSnapshot())) claimed += build->bytes;
    EXPECT_EQ(cache->stats().claimedBytes, claimed); // counted once for two charts
}


// Review fix 1: a decoded budget smaller than one chunk must not refetch forever.
TEST_F(SourceController, ChunksLargerThanTheDecodedBudgetAreFetchedOnce) {
    store.setMaxBytes(1);
    auto &a = chart();
    view(a);
    settle();
    std::set<std::tuple<std::string, int64_t, int64_t>> unique;
    for (const auto &key : requestedKeys()) unique.emplace(key.source, key.levelMs, key.startMs);
    EXPECT_EQ(requestedKeys().size(), unique.size());
    EXPECT_EQ(store.stats().loads, unique.size());
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 5u);
    for (const auto &span : a.latestSnapshot()->spans) EXPECT_TRUE(span.complete);
    charts.clear(); // no chart wants them: the budget applies again
    drain();
    EXPECT_EQ(store.stats().entries, 0u);
}

// A cancelled old-serial rebuild is requested afresh on return.
TEST_F(SourceController, ReturningToATimeframeRestartsACancelledRebuild) {
    auto &a = chart();
    view(a, kMinuteMs);
    settle();
    const SpanSnapshot *visible = nullptr;
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible) visible = &span;
    ASSERT_TRUE(visible);
    const auto g = visible->sources[0].build->key.generations.front();
    const ChunkKey key{g.symbol, g.source, g.levelMs, g.startMs};
    revise(store, key, 2);
    drain();
    ASSERT_FALSE(jobs.empty()); // the rebuild is queued
    view(a, 5 * kMinuteMs);
    settle(); // the old-serial rebuild is cancelled; built content stays retained
    EXPECT_EQ(a.stats().staleResults, 0u);
    view(a, kMinuteMs);
    settle();
    bool checked = false;
    for (const auto &span : a.latestSnapshot()->spans) {
        if (span.rank.tier != SpanTier::Visible) continue;
        EXPECT_TRUE(span.complete);
        for (const auto &source : span.sources)
            for (const auto &gen : source.build->key.generations)
                if (gen.source == key.source && gen.startMs == key.startMs) {
                    EXPECT_EQ(gen.generation, store.generationOf(key));
                    checked = true;
                }
    }
    EXPECT_TRUE(checked);
}

// Review fix 3: claimed images are bounded by the CPU tier across charts.
TEST_F(SourceController, CpuTierDropsTheLowestRanksAcrossCharts) {
    size_t spanBytes = 0; // CPU size of one 1m span (both sources), measured
    {
        auto &x = chart();
        view(x);
        settle();
        for (const auto &span : x.latestSnapshot()->spans)
            if (span.rank.tier == SpanTier::Visible)
                for (const auto &source : span.sources) spanBytes += source.build->bytes;
    }
    charts.clear();
    ASSERT_GT(spanBytes, 0u);
    makeCache(spanBytes * 7 / 2); // room for 3.5 spans; no size hints yet
    // 1-byte estimates: admission cannot foresee the real size.
    auto &a = chart(320ull << 20, 1);
    auto &b = chart(320ull << 20, 1);
    view(a);
    view(b);
    settle();
    const auto stats = cache->stats();
    EXPECT_LE(stats.claimedBytes, cache->maxBytes());
    EXPECT_LE(size_t(stats.liveBytes), cache->maxBytes());
    EXPECT_GT(stats.pressureDrops, 0u);
    for (auto *c : {&a, &b}) {
        const auto snapshot = c->latestSnapshot();
        ASSERT_EQ(snapshot->spans.size(), 3u);
        EXPECT_TRUE(visibleComplete(*snapshot));
        for (const auto &span : snapshot->spans)
            if (span.rank.tier != SpanTier::Visible) EXPECT_EQ(span.rank, (SpanRank{SpanTier::Prefetch, 1}));
    }
    // Stable: size hints now keep the dropped spans out.
    const auto built = cache->stats().builds;
    a.setGpuBudget(320ull << 20);
    b.setGpuBudget(320ull << 20);
    settle();
    EXPECT_EQ(cache->stats().builds, built);
}

// Review fix 3 (S5c hook): uploaded images are released and rebuilt after loss.
TEST_F(SourceController, UploadedImagesAreReleasedAndRebuiltAfterGpuLoss) {
    auto &a = chart();
    view(a);
    settle();
    EXPECT_GT(cache->stats().claimedBytes, 0u);
    EXPECT_EQ(upload(a, 1ull << 30), 10u);
    for (const auto &span : a.latestSnapshot()->spans) {
        EXPECT_TRUE(span.complete);
        for (const auto &source : span.sources) {
            ASSERT_TRUE(source.build);
            EXPECT_FALSE(source.build->gpu); // the node draws its resident copy
        }
    }
    EXPECT_EQ(cache->stats().claimedBytes, 0u);
    EXPECT_EQ(a.stats().releasedImages, 10u);
    EXPECT_EQ(cache->stats().liveBytes, 0) << "no image stays after the upload (the LRU is not shrunk)";
    const auto requests = requestedKeys().size();
    const auto built = cache->stats().builds;
    a.capacity()->report(1ull << 30, {}, true); // QRhi lost
    a.pollCapacity();
    settle();
    EXPECT_EQ(cache->stats().builds, built + 10);
    EXPECT_EQ(requestedKeys().size(), requests); // rebuilt from local chunks
    for (const auto &span : a.latestSnapshot()->spans)
        for (const auto &source : span.sources) EXPECT_TRUE(source.build && source.build->gpu);
}

// S5c: a full node (no credit) still admits prefetch on the side the view moves
// to, by evicting strictly lower-ranked prefetch left behind (the node frees
// what the snapshot stops listing). Without it the stale side kept the cap full.
TEST_F(SourceController, CreditBlockedPrefetchEvictsLowerRankedPrefetchAfterAPan) {
    constexpr size_t estimate = 1024; // below one real build: one victim makes room
    auto &a = chart(1ull << 30, estimate);
    view(a);
    settle();
    const int64_t visible = tiles::tileOfBucket(epoch / kMinuteMs);
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 5u);
    upload(a, 0); // everything resident, the node full
    settle();
    // Pan two tiles left: the old visible tile is right-side prefetch at distance 2.
    a.setView("BTC-USD", kMinuteMs, double(epoch - 2 * tileMs), double(epoch - tileMs));
    settle();
    std::map<int64_t, SpanRank> ranks;
    for (const auto &span : a.latestSnapshot()->spans) ranks[span.id.tile] = span.rank;
    EXPECT_TRUE(ranks.contains(visible - 3)) << "left prefetch at distance 1 admitted";
    EXPECT_FALSE(ranks.contains(visible)) << "right prefetch at distance 2 made room";
    EXPECT_TRUE(ranks.contains(visible - 1)) << "equal rank on the right is never evicted";
    EXPECT_GT(a.stats().evictions, 0u);
}

// S5c node contract addition: a source the node reports missing (its GPU cap
// evicted it after the upload, or a new node never held it) is rebuilt from the
// local chunks when the image was released; the others are untouched.
TEST_F(SourceController, MissingSourcesAreRebuiltFromLocalChunks) {
    auto &a = chart();
    view(a);
    settle();
    ASSERT_EQ(upload(a, 1ull << 30), 10u);
    const auto released = a.latestSnapshot();
    const SpanSourceKey *missing = nullptr;
    for (const auto &span : released->spans)
        if (span.rank.tier == SpanTier::Visible) missing = &span.sources.front().build->key;
    ASSERT_TRUE(missing);
    const SpanSourceKey key = *missing;
    const auto requests = requestedKeys().size();
    const auto built = cache->stats().builds;
    a.capacity()->report(1ull << 30, {}, false, {key});
    a.pollCapacity();
    settle();
    EXPECT_EQ(cache->stats().builds, built + 1) << "only the missing source rebuilt";
    EXPECT_EQ(requestedKeys().size(), requests) << "from local chunks";
    size_t withImage = 0;
    for (const auto &span : a.latestSnapshot()->spans)
        for (const auto &source : span.sources) {
            ASSERT_TRUE(source.build);
            if (source.build->gpu) {
                ++withImage;
                EXPECT_EQ(source.build->key, key);
            }
        }
    EXPECT_EQ(withImage, 1u);
}

// Once the node reports a source uploaded, nothing on the CPU keeps its image:
// not the slot, not the snapshot, not the span-source LRU (plan section 4: "the
// lab keeps them alive after upload; S5 does not"). Every image still alive is
// claimed, and the ledger covers what is pinned: claims, reservations and the
// wanted decoded chunks. Before the fix the LRU kept every uploaded image as an
// unclaimed entry (up to the 256 MiB tier) that no ledger counted.
TEST_F(SourceController, UploadedImagesLeaveTheCacheAndTheLedgerCoversWhatStaysPinned) {
    auto &a = chart();
    view(a);
    settle();
    auto covered = [&] {
        const auto s = cache->stats();
        EXPECT_LE(s.liveBytes, int64_t(s.claimedBytes)) << "every image alive is claimed";
        EXPECT_GE(cache->committedCpuBytes(), s.claimedBytes + s.reservedBytes + store.stats().wantedBytes)
            << "the ledger covers claims, reservations and wanted chunks";
    };
    covered();
    EXPECT_GT(cache->stats().liveBytes, 0);
    EXPECT_EQ(upload(a, 1ull << 30), 10u);
    settle();
    const auto s = cache->stats();
    EXPECT_EQ(s.liveBytes, 0) << "uploaded images are gone from the CPU";
    EXPECT_EQ(s.bytes, 0u) << "the LRU keeps no uploaded image";
    EXPECT_EQ(s.claimedBytes, 0u);
    covered();
    // A second chart on the same view still shares one build per source while
    // both hold it, and releases it after its own upload.
    auto &b = chart();
    view(b);
    settle();
    EXPECT_GT(cache->stats().liveBytes, 0) << "rebuilt for the chart that has not uploaded";
    covered();
    upload(b, 1ull << 30);
    settle();
    EXPECT_EQ(cache->stats().liveBytes, 0);
    covered();
}

// Re-review: a key two charts claim. Once one chart's node uploaded it, the LRU
// never keeps it, whether the other chart uploads later or closes first; its
// own reference keeps the image only while it needs it. No cache shrinking.
TEST_F(SourceController, ASharedImageUploadedByOneChartDiesWhenTheOtherLetsGo) {
    for (const bool otherUploads : {false, true}) {
        SCOPED_TRACE(otherUploads ? "the other chart uploads later" : "the other chart closes first");
        auto &a = chart();
        auto &b = chart();
        view(a);
        view(b);
        settle();
        ASSERT_GT(cache->stats().liveBytes, 0);
        EXPECT_EQ(cache->stats().builds % 10, 0u) << "one shared build per source";
        upload(a, 1ull << 30);
        settle();
        EXPECT_GT(cache->stats().liveBytes, 0) << "B still needs the image for its own upload";
        if (otherUploads) {
            upload(b, 1ull << 30);
            settle();
        } else {
            charts.pop_back(); // B closes before its upload
            drain();
            settle();
        }
        EXPECT_EQ(cache->stats().liveBytes, 0) << "the uploaded image is gone";
        EXPECT_EQ(cache->stats().bytes, 0u) << "and not in the LRU";
        charts.clear();
        drain();
    }
}

// Review fix 4: surviving slots take their new ranks before admission.
TEST_F(SourceController, NewlyVisibleSlotsAreNotEvictedWhenTheViewExpands) {
    constexpr size_t estimate = 64 * 1024, span = 2 * estimate, need = span + (span + 9) / 10;
    // Room for the visible span and four prefetch spans, by estimate.
    auto &a = chart(4 * span + need, estimate);
    view(a);
    drain(); // nothing answered or built: all sizes are estimates
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 5u);
    const int64_t t = tiles::tileOfBucket(epoch / kMinuteMs);
    // The view grows left to four tiles: t-2 and t-1 become visible, t-3 is new.
    a.setView("BTC-USD", kMinuteMs, double(tiles::tileStartMs(t - 3, kMinuteMs)), double(tiles::tileEndMs(t, kMinuteMs)));
    drain();
    EXPECT_EQ(a.stats().evictions, 1u); // only t+2, the farthest prefetch
    std::map<int64_t, SpanRank> ranks;
    for (const auto &s : a.latestSnapshot()->spans) ranks[s.id.tile] = s.rank;
    // Visible spans rank by distance from the centre tile (t - 1).
    constexpr auto V = SpanTier::Visible;
    EXPECT_EQ(ranks, (std::map<int64_t, SpanRank>{{t - 3, {V, 2}}, {t - 2, {V, 1}}, {t - 1, {V, 0}}, {t, {V, 1}},
                                                   {t + 1, {SpanTier::Prefetch, 1}}}));
}

TEST_F(SourceController, DemotedFallbackIsEvictableInTheSameReconcile) {
    size_t oneMinute = 0, fiveMinute = 0; // GPU sizes of the visible spans, measured
    {
        auto &x = chart();
        view(x, kMinuteMs);
        settle();
        oneMinute = visibleUploadBytes(*x.latestSnapshot());
        view(x, 5 * kMinuteMs);
        settle();
        fiveMinute = visibleUploadBytes(*x.latestSnapshot());
    }
    charts.clear();
    makeCache(); // no size hints: the 5m prefetch is estimated
    constexpr size_t estimate = 1u << 20;
    const size_t need = 2 * estimate + (2 * estimate + 9) / 10;
    auto &a = chart(320ull << 20, estimate);
    a.capacity()->reportFree(1ull << 40);
    a.pollCapacity();
    view(a, kMinuteMs);
    settle();
    // Room for the 5m view and one prefetch span, but not also the 1m fallback.
    a.setGpuBudget(fiveMinute + need + oneMinute / 2);
    view(a, 5 * kMinuteMs);
    settle();
    bool prefetch = false;
    for (const auto &span : a.latestSnapshot()->spans) {
        EXPECT_EQ(span.id.tfMs, 5 * kMinuteMs) << spanTierName(span.rank.tier);
        prefetch = prefetch || span.rank.tier == SpanTier::Prefetch;
    }
    EXPECT_TRUE(prefetch);
    EXPECT_GE(a.stats().evictions, 1u);
}

// Review fix 5: hour chunks only where the hour level has data.
TEST_F(SourceController, HourChunksStartWhereHourRollupsStart) {
    const int64_t day = (availableStart / kDayMs + 1) * kDayMs; // first day start after the oldest minute
    hourOldest = day + 5 * kHourMs;
    transport.push(availability(day + 3 * kDayMs, hourOldest, day + 2 * kDayMs));
    drain();
    auto &a = chart();
    a.setView("BTC-USD", kHourMs, double(day), double(day + 2 * kDayMs));
    settle();
    size_t hours = 0;
    for (const auto &key : requestedKeys())
        if (key.levelMs == kHourMs) {
            ++hours;
            EXPECT_GE(key.startMs, day + kDayMs);
        }
    EXPECT_GT(hours, 0u);
    // The first partial day composes from minutes: its hours before the first
    // rollup are data, not recorder gaps.
    bool checked = false;
    for (const auto &column : a.latestSnapshot()->resolution.columns)
        if (column.startMs == day + 2 * kHourMs)
            for (const auto &source : column.sources)
                if (coarse(source.source)) {
                    EXPECT_EQ(source.state, BucketState::Present);
                    checked = true;
                }
    EXPECT_TRUE(checked);
}

// Review fix 6: builds smaller than their estimates return credit at once.
TEST_F(SourceController, SmallerBuildsReturnCreditWithoutANewNodeReport) {
    constexpr size_t estimate = 64 * 1024, span = 2 * estimate, need = span + (span + 9) / 10;
    auto &a = chart(64ull << 20, estimate);
    // One report: the view's estimate plus just under one prefetch span.
    a.capacity()->reportFree(span + need - 1);
    a.pollCapacity();
    const auto epochNow = a.capacity()->capacityEpoch.load();
    view(a);
    drain();
    EXPECT_EQ(a.latestSnapshot()->spans.size(), 1u);
    settle(); // real builds are far smaller than the estimates
    EXPECT_EQ(a.latestSnapshot()->spans.size(), 5u);
    EXPECT_EQ(a.capacity()->capacityEpoch.load(), epochNow);
}

// Review fix 7: a refused replacement must not let the obsolete build publish.
TEST_F(SourceController, SupersededQueuedBuildReleasesAdmissionForItsReplacement) {
    makeCache(256ull << 20, 1);
    auto &a = chart();
    view(a);
    answerAll();
    ASSERT_EQ(jobs.size(), 1u); // the view's first source (queue of one)
    const ChunkKey key{"BTC-USD", std::string(kChunkSources[0].id), kMinuteMs, epoch / kHourMs * kHourMs};
    revise(store, key, 2);
    drain();
    ASSERT_EQ(cache->stats().jobs, 1u); // the replacement took the released admission slot
    ASSERT_EQ(jobs.size(), 2u); // old executor closure is an inert tombstone
    auto job = std::move(jobs.front());
    jobs.pop_front();
    job();
    drain();
    EXPECT_EQ(a.stats().staleResults, 0u);
    for (const auto &span : a.latestSnapshot()->spans)
        for (const auto &source : span.sources)
            if (source.build)
                for (const auto &g : source.build->key.generations)
                    if (g.source == key.source && g.startMs == key.startMs)
                        EXPECT_EQ(g.generation, store.generationOf(key));
    settle();
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

// Default pool path: controllers and the cache go away with builds in flight.
TEST_F(SourceController, DestructionWithBuildsInFlightOnTheRealPool) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;
        int entered = 0;
    } gate;
    auto waitEntered = [&] {
        std::unique_lock lock(gate.mutex);
        gate.changed.wait(lock, [&] { return gate.entered > 0; });
    };
    auto open = [&](bool value) {
        {
            std::scoped_lock lock(gate.mutex);
            gate.open = value;
            gate.entered = 0;
        }
        gate.changed.notify_all();
    };
    SpanSourceCache::Options options; // no executor: the cache's QThreadPool
    options.beforeBuild = [&] {
        std::unique_lock lock(gate.mutex);
        ++gate.entered;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.open; });
    };
    cache = std::make_unique<SpanSourceCache>(options);
    // 1. Controllers destroyed while builds run; completions arrive afterwards.
    view(chart());
    view(chart());
    answerAll();
    waitEntered();
    charts.clear();
    open(true);
    {
        // Completions arrive as queued calls; the watchdog only guards a deadlock.
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(cache.get(), &SpanSourceCache::settled, &loop, [&] {
            if (!cache->stats().jobs) loop.quit();
        });
        watchdog.start(10'000);
        if (cache->stats().jobs) loop.exec();
    }
    ASSERT_EQ(cache->stats().jobs, 0u);
    EXPECT_EQ(cache->stats().claimedBytes, 0u);
    // 2. The cache destroyed right after its builds are released.
    open(false);
    store.clear();
    auto &c = chart();
    c.setView("BTC-USD", kMinuteMs, double(epoch + 20 * tileMs), double(epoch + 21 * tileMs));
    answerAll();
    waitEntered();
    charts.clear();
    open(true);
    cache.reset(); // joins the pool; completions queued for it are discarded
    drain();
    SUCCEED();
}

// Re-review P1: the process-wide CPU ceiling refuses the farthest visible spans.
TEST_F(SourceController, CpuCeilingRefusesTheFarthestVisibleSpansAcrossCharts) {
    size_t spanBytes = 0; // CPU size of one built 1m span's images, measured
    {
        auto &x = chart();
        view(x);
        settle();
        for (const auto &span : x.latestSnapshot()->spans)
            if (span.rank.tier == SpanTier::Visible)
                for (const auto &source : span.sources) spanBytes += source.build->bytes;
    }
    charts.clear();
    makeCache();
    const size_t ceiling = 9 * spanBytes + spanBytes / 2;
    cache->setCpuCeiling(ceiling);
    auto &a = chart(320ull << 20, spanBytes);
    view(a);
    settle();
    auto &b = chart(320ull << 20, spanBytes);
    // A wide view: eight tiles.
    b.setView("BTC-USD", kMinuteMs, double(epoch + 12 * tileMs), double(epoch + 20 * tileMs));
    settle();
    EXPECT_LE(cache->committedCpuBytes(), ceiling);
    EXPECT_LE(store.stats().wantedBytes + cache->pinnedBytes(), ceiling);
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
    const auto snapshot = b.latestSnapshot();
    ASSERT_FALSE(snapshot->refused.empty());
    EXPECT_GT(snapshot->refusedBytes, 0u);
    EXPECT_EQ(b.stats().refused, snapshot->refused.size());
    const int64_t centre = tiles::tileOfBucket((epoch + 16 * tileMs) / kMinuteMs);
    int64_t farthestAdmitted = -1;
    size_t visible = 0;
    for (const auto &span : snapshot->spans)
        if (span.rank.tier == SpanTier::Visible) {
            ++visible;
            EXPECT_TRUE(span.complete);
            farthestAdmitted = std::max(farthestAdmitted, std::abs(span.id.tile - centre));
        }
    EXPECT_GE(visible, 1u);
    EXPECT_EQ(visible + snapshot->refused.size(), 8u);
    for (const auto &id : snapshot->refused) EXPECT_GE(std::abs(id.tile - centre), farthestAdmitted) << id.tile;
}

TEST_F(SourceController, LabelTierCancelsActiveQueryBeforeRefusingVisibleSpans) {
    auto &a = chart();
    a.setView("BTC-USD", kMinuteMs, double(epoch), double(epoch + 3*tileMs));
    settle();
    const auto picture = a.latestSnapshot();
    ASSERT_TRUE(visibleComplete(*picture));
    auto *query = a.cellQuery();
    LabelRequest request{1, picture->version, 0, kMinuteMs, 500, 100, epoch/kMinuteMs, 0, 64, 40, "BTC"};
    query->requestLabels(request, picture);
    settle();
    ASSERT_NE(query->latestLabels(), nullptr);
    ++request.serial;
    query->requestLabels(request, picture); // a queued/running job retains its own scratch charge
    ASSERT_EQ(jobs.size(), 1u);
    const auto before = cache->committedCpuBytes();
    size_t optionalBytes = 0;
    for (const auto *build : builds(*picture, SpanTier::Prefetch)) optionalBytes += build->bytes;
    for (const auto *build : builds(*picture, SpanTier::RecentTf)) optionalBytes += build->bytes;
    // Queue the plan before budgetsChanged's queued optional-query handler:
    // otherwise that handler masks incorrect refusal ordering in the controller.
    a.setView("BTC-USD", kMinuteMs, double(epoch)+1, double(epoch+3*tileMs)-1);
    cache->setCpuCeiling(before - optionalBytes - 1);
    QCoreApplication::sendPostedEvents(&a, QEvent::MetaCall);
    EXPECT_EQ(query->latestLabels(), nullptr) << "the controller must synchronously cancel the label tier";
    const auto after = a.latestSnapshot();
    EXPECT_TRUE(after->refused.empty());
    EXPECT_EQ(builds(*after, SpanTier::Visible), builds(*picture, SpanTier::Visible));
    EXPECT_LE(cache->committedCpuBytes(), cache->cpuCeiling());
    EXPECT_GE(cache->committedCpuBytes(), 64ull << 20) << "do not uncharge the still-running job";
    settle();
    EXPECT_EQ(query->latestLabels(), nullptr) << "the cancelled completion cannot publish";
}

TEST_F(SourceController, ASpanLargerThanTheWholeCeilingIsAdmittedAlone) {
    cache->setCpuCeiling(1);
    auto &a = chart();
    a.setView("BTC-USD", kMinuteMs, double(epoch - tileMs), double(epoch + 2 * tileMs)); // three tiles
    settle();
    const auto snapshot = a.latestSnapshot();
    ASSERT_EQ(snapshot->spans.size(), 1u);
    EXPECT_TRUE(snapshot->spans[0].complete);
    EXPECT_EQ(snapshot->spans[0].id.tile, tiles::tileOfBucket(epoch / kMinuteMs)); // the centre
    EXPECT_EQ(snapshot->refused.size(), 2u);
    EXPECT_GT(a.stats().refusals, 0u);
}

// Re-review P1: decoded chunks stay wanted only while a build needs them.
TEST_F(SourceController, BuiltSpansUnwantTheirSealedChunksAndGpuLossWantsThemAgain) {
    auto &a = chart();
    view(a);
    answerAll(); // chunks in, builds queued
    EXPECT_GT(store.stats().wantedBytes, 0u);
    settle();
    EXPECT_EQ(store.stats().wantedBytes, 0u); // all built; the test chunks are sealed
    EXPECT_EQ(upload(a, 1ull << 30), 10u);
    store.setMaxBytes(1); // unwanted chunks are evictable now
    EXPECT_EQ(store.stats().entries, 0u);
    const auto requests = requestedKeys().size();
    a.capacity()->report(1ull << 30, {}, true); // GPU loss
    a.pollCapacity();
    drain();
    EXPECT_GT(requestedKeys().size(), requests); // wanted again, refetched
    settle();
    for (const auto &span : a.latestSnapshot()->spans)
        for (const auto &source : span.sources) EXPECT_TRUE(source.build && source.build->gpu);
    EXPECT_EQ(store.stats().wantedBytes, 0u);
}

// A source made ready from the cache after demand was computed must not leave
// its chunks wanted (one more reconcile follows a cache hit).
// (S5c: uploaded images no longer stay in the LRU, so a loss rebuilds them from
// the local chunks, as plan section 4 says.)
TEST_F(SourceController, LossRebuildsFromLocalChunksAndLeavesNothingWanted) {
    auto &a = chart();
    view(a);
    settle();
    upload(a, 1ull << 30); // images released, and gone from the LRU
    const auto built = cache->stats().builds;
    const auto requests = requestedKeys().size();
    a.capacity()->report(1ull << 30, {}, true);
    a.pollCapacity();
    settle();
    EXPECT_EQ(cache->stats().builds, built + 10);
    EXPECT_EQ(requestedKeys().size(), requests) << "from local chunks";
    for (const auto &span : a.latestSnapshot()->spans)
        for (const auto &source : span.sources) EXPECT_TRUE(source.build && source.build->gpu);
    EXPECT_EQ(store.stats().wantedBytes, 0u);
}

// Re-review P2: a GPU loss rebuilds the drawn fallback, drops recent-tf.
TEST_F(SourceController, GpuLossRebuildsFallbackAndDropsReleasedRecentTimeframe) {
    auto &a = chart();
    view(a, kMinuteMs);
    settle();
    upload(a, 1ull << 30);
    view(a, 5 * kMinuteMs);
    drain(); // the 5m view is still loading: 1m is the fallback
    a.capacity()->report(1ull << 30, {}, true);
    a.pollCapacity();
    drain();
    // Run only the queued builds; the 5m chunks stay unanswered.
    while (!jobs.empty()) {
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
        drain();
    }
    bool fallback = false;
    for (const auto &span : a.latestSnapshot()->spans) {
        if (span.id.tfMs != kMinuteMs) continue;
        EXPECT_EQ(span.rank.tier, SpanTier::Fallback);
        fallback = true;
        for (const auto &source : span.sources) EXPECT_TRUE(source.build && source.build->gpu);
    }
    EXPECT_TRUE(fallback);
    // The 5m view completes (1m becomes recent-tf); after uploads and another
    // loss, released recent-tf retention is dropped rather than rebuilt.
    settle();
    upload(a, 1ull << 30);
    a.capacity()->report(1ull << 30, {}, true);
    a.pollCapacity();
    settle();
    for (const auto &span : a.latestSnapshot()->spans) {
        EXPECT_EQ(span.id.tfMs, 5 * kMinuteMs);
        for (const auto &source : span.sources) EXPECT_TRUE(source.build && source.build->gpu);
    }
}

// Re-review P2: an availability advance invalidates the build in flight.
TEST_F(SourceController, PlanChangeInvalidatesABuildInFlightBeforeItsNewChunkArrives) {
    const int64_t h0 = epoch / kHourMs * kHourMs;
    transport.push(availability(h0 + kHourMs));
    drain();
    auto &a = chart();
    view(a);
    answerAll(); // builds for the old plan are queued, not run
    ASSERT_FALSE(jobs.empty());
    transport.push(availability(h0 + 2 * kHourMs)); // the visible span now needs chunk h0 + 1h
    drain();
    const auto newChunk = std::find_if(transport.requests.begin() + long(answered), transport.requests.end(),
                                       [&](const auto &r) { return r.starts.front() == h0 + kHourMs; });
    ASSERT_NE(newChunk, transport.requests.end()); // requested, deliberately unanswered
    while (!jobs.empty()) {
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
        drain();
    }
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible) EXPECT_FALSE(span.complete) << "an obsolete plan built as current";
    EXPECT_EQ(a.stats().staleResults, 0u);
    settle();
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

// Re-review P2: freed CPU wakes another chart suppressed for CPU room.
TEST_F(SourceController, FreedCpuCapacityWakesASuppressedChart) {
    auto &a = chart();
    view(a);
    settle();
    size_t spanBytes = 0;
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible)
            for (const auto &source : span.sources) spanBytes += source.build->bytes;
    cache->setMaxBytes(6 * spanBytes + spanBytes / 2); // A's five spans plus B's view
    step();
    auto &b = chart(320ull << 20, spanBytes / 2);
    b.setView("BTC-USD", kMinuteMs, double(epoch + 20 * tileMs), double(epoch + 21 * tileMs));
    settle();
    ASSERT_EQ(b.latestSnapshot()->spans.size(), 1u); // prefetch suppressed by A's claims
    const auto *bPtr = &b;
    charts.erase(charts.begin()); // A closes; nothing else happens
    settle();
    EXPECT_EQ(bPtr->latestSnapshot()->spans.size(), 5u);
}

// Queued work releases immediately, but a cancelled worker remains in the
// ledger until it returns and its owner-thread acknowledgement is delivered.
TEST_F(SourceController, RunningBuildsOfAClosedChartStayInTheLedger) {
    view(chart());
    answerAll();
    ASSERT_FALSE(jobs.empty());
    beforeBuild = [&] {
        charts.clear();
        EXPECT_EQ(cache->stats().jobs, 1u); // only this genuinely started job
        EXPECT_GT(cache->stats().reservedBytes, 0u);
        EXPECT_GT(cache->committedCpuBytes(), cache->stats().reservedBytes);
    };
    runFirst();
    EXPECT_EQ(cache->stats().jobs, 1u); // acknowledgement still queued
    beforeBuild = {};
    drain();
    EXPECT_EQ(cache->stats().jobs, 0u);
    EXPECT_EQ(cache->committedCpuBytes(), 0u);
    settle(); // cancelled executor closures are safe no-ops
}

// Final round P1: a chart pushed over by another chart's keeper sheds its own
// lower-rank work (global relief), never below its keeper.
TEST_F(SourceController, OverCeilingMakesEarlierChartsShedDownToTheirKeepers) {
    auto &a = chart();
    view(a);
    settle();
    size_t spanBytes = 0;
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible)
            for (const auto &source : span.sources) spanBytes += source.build->bytes;
    const size_t ceiling = 5 * spanBytes + spanBytes / 2;
    cache->setCpuCeiling(ceiling);
    step();
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 5u); // A fits with its prefetch
    auto &b = chart(320ull << 20, spanBytes / 2);
    b.setView("BTC-USD", kMinuteMs, double(epoch + 20 * tileMs), double(epoch + 21 * tileMs));
    settle();
    EXPECT_LE(cache->committedCpuBytes(), ceiling);
    EXPECT_LE(store.stats().wantedBytes + cache->pinnedBytes(), ceiling);
    EXPECT_LT(a.latestSnapshot()->spans.size(), 5u); // A shed prefetch
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
    EXPECT_TRUE(visibleComplete(*b.latestSnapshot()));
}

// Final round P2: a reset leaves no phantom commitment or refusal.
TEST_F(SourceController, ResetClearsTheLedgerAndTheRefusals) {
    cache->setCpuCeiling(1);
    auto &a = chart();
    a.setView("BTC-USD", kMinuteMs, double(epoch - tileMs), double(epoch + 2 * tileMs));
    settle();
    ASSERT_EQ(a.latestSnapshot()->refused.size(), 2u);
    ASSERT_GT(cache->committedCpuBytes(), 0u);
    a.setView("", 0, 0, 0); // no chart content
    drain();
    EXPECT_EQ(cache->committedCpuBytes(), 0u);
    EXPECT_TRUE(a.latestSnapshot()->refused.empty());
    EXPECT_EQ(a.stats().refused, 0u);
}

// Final round P2: a newly required chunk changes the build identity at once.
TEST_F(SourceController, ABackfilledChunkChangesTheBuildIdentityBeforeItArrives) {
    const int64_t h0 = epoch / kHourMs * kHourMs;
    // The coarse source's hour level starts long before, so the span's bounds
    // never move; only its minute history is backfilled.
    auto minutesFrom = [&](int64_t oldest) {
        auto a = availability(availableEnd, availableStart - 2 * kDayMs, availableStart);
        for (auto &source : a.sources)
            if (coarse(source.id))
                for (auto &level : source.levels)
                    if (level.levelMs == kMinuteMs) level.oldestMs = oldest;
        return a;
    };
    transport.push(minutesFrom(h0 + kHourMs));
    drain();
    auto &a = chart();
    view(a);
    answerAll(); // the visible coarse source builds from chunk h0 + 1h (queued)
    ASSERT_FALSE(jobs.empty());
    transport.push(minutesFrom(availableStart)); // backfill adds chunk h0
    drain();
    const auto held = std::find_if(transport.requests.begin() + long(answered), transport.requests.end(),
                                   [&](const auto &r) { return coarse(r.source) && r.starts.front() == h0; });
    ASSERT_NE(held, transport.requests.end()); // requested, deliberately unanswered
    while (!jobs.empty()) {
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job();
        drain();
    }
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible) EXPECT_FALSE(span.complete) << "the old plan published as complete";
    settle();
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
}

// Last round A: dropping pending spans that share chunks never raises the total
// (queued jobs release their keys; shared keys keep their measured size).
TEST_F(SourceController, DroppingPendingSpansThatShareChunksNeverRaisesTheTotal) {
    makeCache(256ull << 20, 64); // every build is a queued job
    auto &a = chart();
    view(a);
    answerAll();
    ASSERT_EQ(jobs.size(), 10u); // adjacent spans share hour chunks
    const size_t before = cache->committedCpuBytes();
    cache->setCpuCeiling(before - 1); // force shedding
    drain();
    EXPECT_LT(a.latestSnapshot()->spans.size(), 5u); // cancellation frees enough before reaching the keeper
    EXPECT_FALSE(a.latestSnapshot()->spans.empty());
    EXPECT_LE(cache->committedCpuBytes(), cache->cpuCeiling());
    EXPECT_LT(cache->committedCpuBytes(), before);
}

// Last round A: a job and a surviving span that share chunks count them once.
TEST_F(SourceController, AJobAndASpanSharingChunksCountThemOnce) {
    makeCache(256ull << 20, 64);
    view(chart());
    answerAll();
    ASSERT_EQ(jobs.size(), 10u);
    const size_t firstChartAndJobs = cache->committedCpuBytes();
    ASSERT_GT(firstChartAndJobs, 0u);
    view(chart()); // same chunks: second chart joins, neither the chunks nor jobs count twice
    drain();
    EXPECT_EQ(cache->stats().jobs, 10u);
    EXPECT_EQ(cache->committedCpuBytes(), firstChartAndJobs);
}

// Last round B: a keeper whose jobs exist before its commit still makes the
// other charts shed.
TEST_F(SourceController, AKeeperWithJobsBeforeItsCommitStillMakesOthersShed) {
    auto &a = chart();
    view(a);
    settle();
    size_t spanBytes = 0;
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible)
            for (const auto &source : span.sources) spanBytes += source.build->bytes;
    const int64_t far = epoch + 20 * tileMs;
    {   // Warm B's chunks in the store (not its builds): its first pass requests jobs.
        auto &x = chart();
        x.setView("BTC-USD", kMinuteMs, double(far), double(far + tileMs));
        settle();
    }
    charts.pop_back();
    drain();
    cache->setMaxBytes(0); // no cached builds for B
    cache->setMaxBytes(256ull << 20);
    const size_t ceiling = 5 * spanBytes + spanBytes / 2;
    cache->setCpuCeiling(ceiling);
    step();
    ASSERT_EQ(a.latestSnapshot()->spans.size(), 5u);
    auto &b = chart(320ull << 20, spanBytes / 2);
    b.setView("BTC-USD", kMinuteMs, double(far), double(far + tileMs));
    settle();
    EXPECT_LT(a.latestSnapshot()->spans.size(), 5u); // A shed
    EXPECT_LE(cache->committedCpuBytes(), ceiling);
    EXPECT_TRUE(visibleComplete(*a.latestSnapshot()));
    EXPECT_TRUE(visibleComplete(*b.latestSnapshot()));
}

// Last round C: clearing failures does not change the plan identity.
TEST_F(SourceController, ClearedFailuresKeepThePlanAndTheBuild) {
    const int64_t h0 = epoch / kHourMs * kHourMs;
    const ChunkKey failing{"BTC-USD", std::string(kChunkSources[0].id), kMinuteMs, h0};
    auto &a = chart();
    view(a);
    for (int i = 0; i < 1000; ++i) { // answer everything; the failing chunk errors
        drain();
        if (answered == transport.requests.size()) break;
        while (answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t k = 0; k < request.starts.size(); ++k) {
                if (request.key(k) == failing) transport.error(request.id, request.key(k), QStringLiteral("build_failed"));
                else transport.reply(request.id, body(request.key(k)));
            }
        }
    }
    settle();
    const SpanSnapshot *visible = nullptr;
    for (const auto &span : a.latestSnapshot()->spans)
        if (span.rank.tier == SpanTier::Visible) visible = &span;
    ASSERT_TRUE(visible && visible->complete); // built without the failed chunk
    std::vector<const SpanSourceBuild *> built;
    for (const auto &source : visible->sources) {
        built.push_back(source.build.get());
        for (const auto &g : source.build->key.generations)
            EXPECT_FALSE(g.source == failing.source && g.startMs == failing.startMs); // really left out
    }
    const size_t requestsBefore = transport.requests.size();
    const auto builds = cache->stats().builds;
    transport.push(availability()); // clears the controller's failures; same plan
    drain();
    for (const auto &span : a.latestSnapshot()->spans) {
        if (span.rank.tier != SpanTier::Visible) continue;
        EXPECT_TRUE(span.complete) << "clearing failures invalidated the build";
        for (size_t i = 0; i < span.sources.size(); ++i) EXPECT_EQ(span.sources[i].build.get(), built[i]);
    }
    EXPECT_EQ(cache->stats().builds, builds);
    EXPECT_TRUE(jobs.empty());
    // The failure retry is separate: the chunk is requested again.
    bool retried = false;
    for (size_t r = requestsBefore; r < transport.requests.size(); ++r)
        for (size_t k = 0; k < transport.requests[r].starts.size(); ++k)
            retried = retried || transport.requests[r].key(k) == failing;
    EXPECT_TRUE(retried);
}

// Two CPU-suppressed charts share pending chunks with different size hints:
// the ledger must settle, not bounce between the hints forever.
TEST_F(SourceController, SharedPendingChunksWithDifferentHintsSettle) {
    auto &a = chart();
    view(a);
    settle(); // A measured its chunks: its hints are their real (small) sizes
    upload(a, 1ull << 30);
    store.setMaxBytes(1); // the (unwanted, sealed) chunks leave the store
    ASSERT_EQ(store.stats().entries, 0u);
    a.capacity()->report(1ull << 30, {}, true); // GPU loss: A wants its chunks again
    a.pollCapacity();
    auto &b = chart(); // the same view without hints: 4 MiB estimates
    view(b);
    cache->setCpuCeiling(1); // both shed down to their keepers (CPU-suppressed)
    for (int i = 0; i < 5; ++i) drain(); // the chunk requests stay unanswered
    ASSERT_GT(transport.requests.size(), answered);
    const auto ra = a.stats().reconciles, rb = b.stats().reconciles;
    for (int i = 0; i < 5; ++i) drain();
    EXPECT_EQ(a.stats().reconciles, ra);
    EXPECT_EQ(b.stats().reconciles, rb);
}
} // namespace
