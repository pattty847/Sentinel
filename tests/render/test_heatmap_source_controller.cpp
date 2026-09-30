#include "protocol/SentinelStreamClient.hpp" // shared-frame metatype
#include "render/heatmap/HeatmapSourceController.hpp"
#include "../servermodel/FakeChunkTransport.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <gtest/gtest.h>
#include <climits>
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

ChunkAvailability availability() {
    ChunkAvailability a;
    a.symbol = "BTC-USD";
    a.chunkWireVersion = kChunkWireVersion;
    for (const auto &source : kChunkSources) {
        protocol::chunkwire::SourceInfo s;
        s.id = source.id;
        s.latestGrid = protocol::chunkwire::GridInfo{};
        s.latestGrid->priceScale = 100;
        s.levels.push_back({kMinuteMs, kHourMs, availableEnd, availableStart, availableEnd - kMinuteMs});
        a.sources.push_back(std::move(s));
    }
    return a;
}
// Fine source: $1 rows over $95-$105. Coarse source: $5 rows over $0-$200.
ChunkFramePtr body(const ChunkKey &key, uint64_t revision = 1) {
    auto frame = std::make_shared<ChunkFrame>();
    frame->key = key;
    frame->state = {true, key.startMs + kHourMs, revision};
    frame->contentHash = revision;
    const bool isCoarse = coarse(key.source);
    const int64_t tick = isCoarse ? 500 : 100, lo = isCoarse ? 0 : 9500, hi = isCoarse ? 20000 : 10500;
    frame->columns = {key.symbol, std::string(findChunkSource(key.source)->hmc2Layer), key.levelMs, key.startMs,
                      key.startMs + kHourMs, {}, {}};
    frame->columns.scannedRanges = {{key.startMs, key.startMs + kHourMs}};
    for (auto t = key.startMs; t < key.startMs + kHourMs; t += key.levelMs) {
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

    void SetUp() override {
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
    void makeCache(size_t maxBytes = 256ull << 20, size_t maxJobs = 8) {
        SpanSourceCache::Options options;
        options.maxBytes = maxBytes;
        options.maxJobs = maxJobs;
        options.executor = [this](std::function<void()> job, int) { jobs.push_back(std::move(job)); };
        cache = std::make_unique<SpanSourceCache>(options);
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
    // No LRU: only recent-tf retention can keep the previous tf's sources.
    makeCache(1);
    auto &a = chart();
    view(a, kMinuteMs);
    settle();
    const auto oneMinute = builds(*a.latestSnapshot(), SpanTier::Visible);
    ASSERT_EQ(oneMinute.size(), 2u);
    view(a, 5 * kMinuteMs);
    settle();
    const auto middle = a.latestSnapshot();
    EXPECT_EQ(middle->tfMs, 5 * kMinuteMs);
    EXPECT_EQ(builds(*middle, SpanTier::RecentTf), oneMinute);
    const auto requests = requestedKeys().size();
    const auto built = cache->stats().builds;
    // Switch back: the visible spans are published on the first reconcile,
    // before any build job runs or any chunk is answered.
    view(a, kMinuteMs);
    drain();
    const auto back = a.latestSnapshot();
    EXPECT_EQ(back->tfMs, kMinuteMs);
    EXPECT_TRUE(visibleComplete(*back));
    EXPECT_EQ(builds(*back, SpanTier::Visible), oneMinute);
    EXPECT_EQ(requestedKeys().size(), requests);
    settle();
    EXPECT_EQ(requestedKeys().size(), requests); // zero requests overall
    // Only the 1m prefetch spans (not retained, no LRU) were rebuilt.
    EXPECT_EQ(cache->stats().builds, built + 8);
    EXPECT_EQ(builds(*a.latestSnapshot(), SpanTier::RecentTf).size(), 2u); // the 5m view
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
    // Freed bytes below bytes + 10% admit nothing.
    a.capacity()->reportFree(need - 1);
    a.pollCapacity();
    settle();
    EXPECT_EQ(a.latestSnapshot()->spans.size(), 1u);
    EXPECT_EQ(cache->stats().builds, 2u);
    // Same view and budget; only the capacity epoch changes. The nearest
    // prefetch span returns, strictly by rank.
    a.capacity()->reportFree(need);
    a.pollCapacity();
    settle();
    const auto admitted = a.latestSnapshot();
    ASSERT_EQ(admitted->spans.size(), 2u);
    EXPECT_EQ(admitted->spans[1].rank, (SpanRank{SpanTier::Prefetch, 1}));
    EXPECT_TRUE(admitted->spans[1].complete);
    EXPECT_EQ(cache->stats().builds, 4u);
    EXPECT_EQ(a.stats().suppressed, 3u);
    // Static view: repeated epoch bumps without new room change nothing.
    const auto stable = cache->stats();
    const auto stats = a.stats();
    for (int i = 0; i < 20; ++i) {
        a.capacity()->reportFree(need - 1);
        a.pollCapacity();
        settle();
    }
    EXPECT_EQ(cache->stats().builds, stable.builds);
    EXPECT_EQ(cache->stats().evictions, stable.evictions);
    EXPECT_EQ(a.stats().evictions, 0u);
    EXPECT_EQ(a.stats().admissions, stats.admissions);
    EXPECT_EQ(a.latestSnapshot()->version, admitted->version);
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

TEST_F(SourceController, OlderSerialBuildResultsAreDropped) {
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
    EXPECT_GT(a.stats().staleResults, 0u);
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
    EXPECT_GT(a.stats().staleResults, 0u);
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

TEST_F(SourceController, BudgetsAreConfigurableAndPublishedBuildsStayShared) {
    HeatmapBudgets budgets;
    budgets.cpuCeiling = 1;
    EXPECT_FALSE(HeatmapSourceController::applyBudgets(budgets, store, *cache));
    budgets.cpuCeiling = 1ull << 30;
    budgets.spanSources = 1;
    EXPECT_TRUE(HeatmapSourceController::applyBudgets(budgets, store, *cache));
    EXPECT_EQ(store.stats().maxBytes, budgets.decodedChunks);
    auto &a = chart();
    auto &b = chart();
    view(a);
    view(b);
    settle();
    EXPECT_LE(cache->stats().bytes, 1u);
    EXPECT_EQ(cache->stats().builds, 10u); // the weak registry still shares
    EXPECT_EQ(builds(*a.latestSnapshot()), builds(*b.latestSnapshot()));
}
} // namespace
