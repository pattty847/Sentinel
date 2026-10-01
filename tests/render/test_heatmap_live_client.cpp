#include "protocol/SentinelStreamClient.hpp"
#include "render/heatmap/HeatmapSourceController.hpp"
#include "heatmap/TimeComposer.hpp"
#include "heatmap/BinCell.hpp"
#include "servermodel/BookRecorder.hpp"
#include "servermodel/RecordingLive.hpp"
#include <QTemporaryDir>
#include "../servermodel/FakeChunkTransport.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <gtest/gtest.h>
#include <deque>
#include <set>
#include <thread>

namespace {
using namespace heatmap;
constexpr int64_t base = (recording::kHmc2MinMs / kDayMs + 4) * kDayMs;
const std::string symbol = "BTC-USD", source = "hmc2.deep";
int64_t minute(int n) { return base + n * kMinuteMs; }
int64_t hourOf(int64_t t) { return recording::floorDiv(t, kHourMs) * kHourMs; }
void drainLive() { for (int i = 0; i < 48; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
SparseColumn col(int64_t t, uint64_t observed = kMinuteMs, double size = 3) {
    NativeColumn n;
    n.grid = {1, 100, 100};
    n.baseRow = 95;
    n.observedMs = observed;
    n.coverage[0] = {{95, 105, observed}};
    n.coverage[1] = n.coverage[0];
    n.entries = {{0, recording::encodeSize(size)}, {packRowSide(101, 95, true), recording::encodeSize(size * 2)}};
    return {t, observed, observed < kMinuteMs ? recording::kPartial : 0u, {std::move(n)}};
}
ChunkAvailability available(int64_t cutoff = minute(10), bool twoSources = false) {
    ChunkAvailability a;
    a.symbol = symbol;
    a.chunkWireVersion = kChunkWireVersion;
    for (const auto &s : kChunkSources) if (twoSources || s.id == source) {
        protocol::chunkwire::SourceInfo info;
        info.id = s.id;
        info.latestGrid = protocol::chunkwire::GridInfo{};
        info.latestGrid->priceScale = 100;
        info.levels.push_back({kMinuteMs, kHourMs, cutoff, base, cutoff - kMinuteMs});
        a.sources.push_back(std::move(info));
    }
    return a;
}
ChunkFramePtr chunk(const ChunkKey &key, int64_t cutoff, uint64_t revision = 1, double size = 3) {
    auto f = std::make_shared<ChunkFrame>();
    f->key = key;
    const auto end = key.startMs + kHourMs;
    const auto through = std::clamp(cutoff, key.startMs, end);
    f->state = {through == end, through, revision};
    f->contentHash = uint64_t(through) + revision;
    f->columns = {symbol, std::string(findChunkSource(key.source)->hmc2Layer), kMinuteMs, key.startMs, end, {}, {}};
    if (through > key.startMs) f->columns.scannedRanges = {{key.startMs, through}};
    for (auto t = std::max(base, key.startMs); t < through; t += kMinuteMs) f->columns.columns.push_back(col(t, kMinuteMs, size));
    return f;
}
ChunkFramePtr tail(int first, int open, int committed, uint64_t revision, uint64_t observed = 1000,
                   std::string id = source) {
    auto f = std::make_shared<ChunkFrame>();
    f->kind = ChunkKind::LiveColumn;
    f->key = {symbol, id, kMinuteMs, hourOf(minute(open))};
    f->state = {false, minute(committed), revision};
    f->columns = {symbol, std::string(findChunkSource(id)->hmc2Layer), kMinuteMs, minute(first), minute(open + 1), {}, {}};
    for (int m = first; m <= open; ++m) {
        auto c = col(minute(m), m == open ? observed : kMinuteMs);
        if (m >= committed) c.flags |= recording::kProvisional;
        f->columns.columns.push_back(std::move(c));
    }
    f->columns.scannedRanges = {{minute(first), minute(open + 1)}};
    return f;
}
std::shared_ptr<const StoredChunk> put(ChunkStore &store, ChunkFramePtr frame) {
    return store.put(frame->key, std::shared_ptr<const SparseColumns>(frame, &frame->columns), frame->state, frame->contentHash);
}
class LiveClient : public testing::Test {
protected:
    int argc = 1;
    char name[16] = "live-client";
    char *argv[2]{name, nullptr};
    QCoreApplication app{argc, argv};
    ChunkStore store;
    FakeChunkTransport transport;
    ChunkFetcher fetcher{store, transport};
    int64_t now = 0, cutoff = minute(10);
    uint64_t chunkRevision = 1;
    // Cadence tests must not enter cost backoff because the host was descheduled.
    // The backoff cases override this with an explicitly measured 6 ms update.
    std::function<int64_t()> composeClock = [] { return int64_t(0); };
    int coalesceMs = 0; // live frames compose at once unless a test sets the window
    std::deque<std::function<void()>> jobs;
    std::unique_ptr<SpanSourceCache> cache;
    std::vector<std::unique_ptr<HeatmapSourceController>> charts;
    size_t answered = 0;
    void SetUp() override {
        SpanSourceCache::Options options;
        options.executor = [this](std::function<void()> job, int) { jobs.push_back(std::move(job)); };
        cache = std::make_unique<SpanSourceCache>(options);
        transport.goOnline(); transport.push(available(cutoff)); drainLive();
    }
    void TearDown() override { charts.clear(); jobs.clear(); cache.reset(); drainLive(); }
    HeatmapSourceController &chart(int64_t tf = kMinuteMs, int viewEnd = 80) {
        HeatmapSourceController::Options options;
        options.capacityPollMs = 0;
        options.nowMs = [this] { return now; };
        options.composeNowNs = composeClock;
        options.liveCoalesceMs = coalesceMs;
        charts.push_back(std::make_unique<HeatmapSourceController>(store, fetcher, *cache, options));
        charts.back()->setView(symbol, tf, double(base), double(minute(viewEnd)));
        drainLive();
        return *charts.back();
    }
    void answer() {
        for (int round = 0; round < 50; ++round) {
            drainLive();
            if (answered == transport.requests.size()) return;
            while (answered < transport.requests.size()) {
                const auto r = transport.requests[answered++];
                for (size_t i = 0; i < r.starts.size(); ++i) transport.reply(r.id, chunk(r.key(i), cutoff, chunkRevision));
            }
        }
        FAIL() << "chunk requests did not settle";
    }
    void runJobs() {
        for (int round = 0; round < 50; ++round) {
            drainLive();
            if (jobs.empty()) return;
            auto queued = std::move(jobs); jobs.clear();
            for (auto &job : queued) job();
        }
        FAIL() << "builds did not settle";
    }
    void settle() { for (int i = 0; i < 4; ++i) { answer(); runJobs(); } }
    void advance(HeatmapSourceController &c, int ms = 1000) { now += ms; c.pollLive(); settle(); }
    quint64 sub() { EXPECT_FALSE(transport.liveRequests.empty()); return transport.liveRequests.back().id; }
    void send(ChunkFramePtr f) { transport.replyLive(sub(), std::move(f)); drainLive(); }
    const LiveSourceSnapshot &live(HeatmapSourceController &c) {
        EXPECT_TRUE(c.latestLive());
        return c.latestLive()->sources.at(0);
    }
    std::vector<SpanSourceKey> uploadKeys(HeatmapSourceController &c) {
        std::vector<SpanSourceKey> keys;
        for (const auto &span : c.latestSnapshot()->spans)
            for (const auto &s : span.sources) if (s.build) keys.push_back(s.build->key);
        return keys;
    }
    void upload(HeatmapSourceController &c) {
        c.capacity()->report(300ull << 20, uploadKeys(c)); c.pollCapacity(); drainLive();
    }
    void coarseRolloverKeepsPreviousBucket(int tfMinutes) {
        const auto tf = tfMinutes * kMinuteMs;
        cutoff = minute(tfMinutes - 1);
        transport.push(available(cutoff)); drainLive();
        auto &c = chart(tf, tfMinutes * 2); settle(); upload(c); settle();
        // Model what the node can draw, not merely a replacement SpanSet that
        // the controller published but the node has not uploaded yet.
        auto drawn = c.latestSnapshot();
        auto historyHasPrevious = [](const SpanSet &set) {
            for (const auto &column : set.resolution.columns) if (column.startMs == base)
                for (const auto &s : column.sources) if (s.state == BucketState::Present) return true;
            return false;
        };
        ASSERT_FALSE(historyHasPrevious(*drawn));
        send(tail(tfMinutes - 1, tfMinutes - 1, tfMinutes - 1, 1, kMinuteMs)); settle();
        ASSERT_TRUE(c.latestLive());
        ASSERT_EQ(bucketState(*live(c).columns, base), BucketState::Present);
        unsigned checked = 0;
        auto checkDrawnUnion = [&] {
            ++checked;
            bool liveHasPrevious = false;
            for (const auto &s : c.latestLive()->sources) {
                if (bucketState(*s.columns, base) != BucketState::Present) continue;
                liveHasPrevious = true;
                const auto column = std::find_if(s.columns->columns.begin(), s.columns->columns.end(),
                                                 [](const auto &v) { return v.bucketStartMs == base; });
                ASSERT_NE(column, s.columns->columns.end());
                EXPECT_EQ(column->observedMs, uint64_t(tf)); // every minute once
            }
            EXPECT_TRUE(historyHasPrevious(*drawn) || liveHasPrevious)
                << "previous bucket would draw loading at tf=" << tf;
        };
        checkDrawnUnion();
        const auto watch = QObject::connect(&c, &HeatmapSourceController::liveChanged, &c, checkDrawnUnion);
        // The new bucket has opened while the server cutoff is still inside
        // the previous bucket. Both the per-source and common-L caps must wait.
        send(tail(tfMinutes - 1, tfMinutes, tfMinutes - 1, 2)); advance(c);
        EXPECT_EQ(live(c).startMs, base);
        checkDrawnUnion();
        cutoff = minute(tfMinutes); ++chunkRevision;
        send(tail(tfMinutes - 1, tfMinutes, tfMinutes, 3)); settle(); advance(c);
        EXPECT_TRUE(historyHasPrevious(*c.latestSnapshot()));
        EXPECT_FALSE(historyHasPrevious(*drawn)); // replacement built, upload deliberately held
        EXPECT_EQ(live(c).startMs, base);
        checkDrawnUnion();
        send(tail(tfMinutes, tfMinutes + 1, tfMinutes, 4)); advance(c);
        EXPECT_EQ(live(c).startMs, base);
        checkDrawnUnion();
        drawn = c.latestSnapshot();
        upload(c); advance(c);
        EXPECT_EQ(live(c).startMs, minute(tfMinutes));
        EXPECT_EQ(bucketState(*live(c).columns, base), BucketState::NotLoaded);
        checkDrawnUnion(); // history takes over only after the upload report
        EXPECT_GE(checked, 8u);
        QObject::disconnect(watch);
    }
};

TEST_F(LiveClient, OneSubscriptionAcrossTwoChartsAndLastReleaseUnsubscribes) {
    auto &a = chart(); auto &b = chart(5 * kMinuteMs);
    ASSERT_EQ(transport.liveRequests.size(), 1u);
    EXPECT_EQ(transport.liveRequests.front().symbol, symbol);
    a.setView("", 0, 0, 0); drainLive();
    EXPECT_TRUE(transport.liveUnsubscribes.empty());
    b.setView("", 0, 0, 0); drainLive();
    ASSERT_EQ(transport.liveUnsubscribes.size(), 1u);
    EXPECT_EQ(transport.liveUnsubscribes.front(), symbol);
}
TEST_F(LiveClient, LiveFramesPreserveChunkBackoffAndTerminalFailures) {
    for (const bool cached : {false, true}) for (const auto &code : {"busy", "timeout", "unavailable", "build_failed", "store_rejected"}) {
        SCOPED_TRACE(testing::Message() << "cached=" << cached << " code=" << code);
        ChunkStore local;
        FakeChunkTransport wire;
        int64_t clock = 0;
        ChunkFetcher::Options options;
        options.nowMs = [&] { return clock; };
        options.retryBaseMs = options.retryMaxMs = 5000;
        ChunkFetcher client(local, wire, options);
        const ChunkKey key{symbol, source, kMinuteMs, base};
        if (cached) put(local, chunk(key, minute(9)));
        wire.goOnline(); wire.push(available()); drainLive();
        client.wantLive(1, symbol);
        const auto subscription = wire.liveRequests.back().id;
        wire.replyLive(subscription, tail(10, 10, 10, 1)); drainLive();
        ASSERT_EQ(wire.requests.size(), 1u);
        wire.error(wire.requests.back().id, key, QString::fromLatin1(code)); drainLive();
        for (int i = 1; i <= 4; ++i) {
            clock = i * 1000;
            wire.replyLive(subscription, tail(10, 10, 10, i + 1)); drainLive();
            client.pump(); drainLive();
            EXPECT_EQ(wire.requests.size(), 1u);
        }
        clock = 5000; client.pump(); drainLive();
        const bool retryable = std::string(code) == "busy" || std::string(code) == "timeout";
        EXPECT_EQ(wire.requests.size(), retryable ? 2u : 1u);
    }
}
TEST_F(LiveClient, RefusedSubscriptionRetriesWithBackoffAndIgnoresOldEpoch) {
    ChunkStore local;
    FakeChunkTransport wire;
    int64_t clock = 0;
    ChunkFetcher::Options options;
    options.nowMs = [&] { return clock; };
    ChunkFetcher client(local, wire, options);
    wire.goOnline(); wire.push(available()); drainLive();
    client.wantLive(1, symbol);
    const auto first = wire.liveRequests.back().id;
    wire.error(first, {}, QStringLiteral("busy")); drainLive();
    wire.replyLive(first, tail(10, 10, 10, 99)); drainLive();
    EXPECT_EQ(client.live(symbol).front()->revision, 0u);
    clock = 99; client.pump(); drainLive(); EXPECT_EQ(wire.liveRequests.size(), 1u);
    clock = 100; client.pump(); drainLive(); ASSERT_EQ(wire.liveRequests.size(), 2u);
    wire.error(wire.liveRequests.back().id, {}, QStringLiteral("refused")); drainLive();
    clock = 299; client.pump(); drainLive(); EXPECT_EQ(wire.liveRequests.size(), 2u);
    clock = 300; client.pump(); drainLive(); ASSERT_EQ(wire.liveRequests.size(), 3u);
    wire.replyLive(wire.liveRequests.back().id, tail(10, 10, 10, 1)); drainLive();
    EXPECT_EQ(client.live(symbol).front()->revision, 1u);
    client.releaseLive(1);
    clock = 10000; client.pump(); drainLive(); EXPECT_EQ(wire.liveRequests.size(), 3u);
}
TEST_F(LiveClient, ReconnectNeedsFreshAvailabilitySinceStoredCutoffAndNewEpochAcceptsBackwardsRevision) {
    auto &c = chart(); settle(); send(tail(10, 10, 10, 90)); settle();
    ASSERT_TRUE(c.latestLive());
    const auto oldSub = sub();
    transport.holdLive(oldSub, tail(10, 10, 10, 99, 9000));
    const auto frozen = c.latestLive();
    transport.goOffline(); drainLive();
    transport.push(available()); drainLive(); // availability before connected cannot unlock
    EXPECT_EQ(transport.liveRequests.size(), 1u);
    transport.goOnline(); drainLive();
    EXPECT_EQ(transport.liveRequests.size(), 1u);
    EXPECT_EQ(c.latestLive(), frozen);
    transport.push(available()); drainLive();
    ASSERT_EQ(transport.liveRequests.size(), 2u);
    EXPECT_EQ(transport.liveRequests.back().sinceMs, cutoff);
    EXPECT_NE(sub(), oldSub);
    transport.releaseLive(); drainLive();
    EXPECT_EQ(fetcher.live(symbol).front()->revision, 90u);
    send(tail(10, 10, 10, 1, 2000)); advance(c);
    EXPECT_EQ(live(c).revision, 1u);
    EXPECT_EQ(live(c).columns->columns.back().observedMs, 2000u);
    send(tail(10, 10, 10, 1, 3000)); advance(c);
    EXPECT_EQ(live(c).columns->columns.back().observedMs, 2000u); // duplicate in this epoch ignored
}
TEST_F(LiveClient, AheadFrameImmediatelyRevalidatesWithHashAndFollowsAnOlderInflightReply) {
    auto &c = chart(); settle();
    const ChunkKey key{symbol, source, kMinuteMs, base};
    const auto hash = store.cached(key)->contentHash;
    const auto count = transport.requests.size();
    send(tail(10, 11, 11, 1));
    ASSERT_GT(transport.requests.size(), count);
    const auto request = transport.requests.back();
    const auto at = std::find(request.starts.begin(), request.starts.end(), base);
    ASSERT_NE(at, request.starts.end());
    EXPECT_EQ(request.hashes[size_t(at - request.starts.begin())], hash);
    send(tail(10, 12, 12, 2)); // another commit while the request is in flight
    transport.reply(request.id, chunk(key, minute(11), 2)); drainLive();
    ASSERT_GT(transport.requests.size(), count + 1);
    EXPECT_EQ(transport.requests.back().hashes[0], uint64_t(minute(11)) + 2);
    cutoff = minute(12); chunkRevision = 3; settle();
    EXPECT_EQ(store.cached(key)->committedThroughMs, cutoff);
    EXPECT_TRUE(c.latestLive());
}

TEST_F(LiveClient, HandoverFinalBeforeRevisionRevisionBeforeFinalAndLostFinal) {
    for (int order = 0; order < 3; ++order) {
        SCOPED_TRACE(order);
        ChunkStore local;
        FakeChunkTransport wire;
        ChunkFetcher client(local, wire);
        wire.goOnline(); wire.push(available()); drainLive();
        client.wantLive(1, symbol);
        auto accept = [&](ChunkFramePtr frame) {
            wire.replyLive(wire.liveRequests.back().id, std::move(frame)); drainLive();
        };
        auto state = [&] { return client.live(symbol).front(); };
        LiveComposer composer;
        const ChunkKey key{symbol, source, kMinuteMs, base};
        auto stored = put(local, chunk(key, minute(10)));
        // A missing final scenario starts without the provisional too, proving
        // it cannot be fabricated from the bounding tail range.
        if (order != 2) accept(tail(10, 10, 10, 1, kMinuteMs));
        else accept(tail(11, 11, 10, 1));
        auto check = [&](bool chunkOwns, bool known) {
            const auto output = composer.compose(*state(), {stored}, kMinuteMs, minute(10)).columns;
            EXPECT_EQ(bucketState(output, minute(10)), known ? BucketState::Present : BucketState::NotLoaded);
            if (known) {
                ASSERT_FALSE(output.columns.empty());
                const auto &column = output.columns.front();
                EXPECT_EQ(column.observedMs, uint64_t(kMinuteMs)); // never counted twice
                const auto expected = col(minute(10), kMinuteMs, chunkOwns ? 7 : 3);
                EXPECT_EQ(recording::encodeSize(double(entryNumerator(column.native.front(), 0) /
                          column.native.front().observedMs)), expected.native.front().entries.front().code);
            }
        };
        check(false, order != 2);
        if (order == 0) { accept(tail(10, 11, 11, 2)); check(false, true); }
        stored = put(local, chunk(key, minute(11), 2, 7));
        // Composer's cutoff rule must work even before the queued trim arrives.
        check(true, true);
        drainLive(); EXPECT_FALSE(state()->minutes.contains(minute(10)));
        if (order == 1) { accept(tail(10, 11, 11, 2)); check(true, true); }
        if (order == 2) { accept(tail(11, 11, 11, 2)); check(true, true); }
    }
}

TEST_F(LiveClient, FormingAtOneFiveAndSixtyMinutesMatchesAllMinuteBinOracleAndCachesPrefix) {
    for (const int tfMinutes : {1, 5, 60}) {
        SCOPED_TRACE(tfMinutes);
        const auto tf = tfMinutes * kMinuteMs;
        ChunkStore local;
        LiveEdge edge(symbol, source);
        LiveComposer composer;
        const int committed = tfMinutes == 1 ? 10 : tfMinutes - 3;
        const int open = tfMinutes == 1 ? 12 : tfMinutes - 2;
        const int begin = tfMinutes == 1 ? 10 : 0;
        const ChunkKey key{symbol, source, kMinuteMs, base};
        const auto stored = put(local, chunk(key, minute(committed)));
        ASSERT_TRUE(edge.accept(tail(committed, open, committed, 1, 1200), local));
        composer.compose(*edge.snapshot(), {stored}, tf, minute(begin));
        ASSERT_TRUE(edge.accept(tail(committed, open, committed, 2, 2300), local));
        const auto updated = composer.compose(*edge.snapshot(), {stored}, tf, minute(begin));
        EXPECT_EQ(updated.composedBuckets, 1u);
        EXPECT_EQ(updated.committedPieces, 0u); // neither committed nor frozen pending prefix re-aggregated
        auto all = tail(begin, open, committed, 2, 2300)->columns;
        for (int i = begin; i < committed; ++i) all.columns[i - begin].flags &= ~recording::kProvisional;
        ComposeOptions options; options.forming = true;
        const SparseColumns *ptr = &all;
        const auto oracle = compose(std::span(&ptr, 1), tf, options);
        ASSERT_EQ(updated.columns.columns.size(), oracle.columns.size());
        for (size_t i = 0; i < oracle.columns.size(); ++i) {
            EXPECT_EQ(updated.columns.columns[i].observedMs, oracle.columns[i].observedMs);
            for (const double tick : {1.0, 5.0}) {
                const auto a = binColumn(updated.columns.columns[i], 90, 110, tick);
                const auto b = binColumn(oracle.columns[i], 90, 110, tick);
                ASSERT_EQ(a.size(), b.size());
                for (size_t row = 0; row < a.size(); ++row) {
                    EXPECT_EQ(a[row].code, b[row].code); EXPECT_EQ(a[row].valid, b[row].valid);
                }
            }
        }
        const auto defaultHistory = compose(std::span(&ptr, 1), tf);
        if (tfMinutes > 1) EXPECT_TRUE(defaultHistory.columns.empty()); // default keeps complete only
    }
}
TEST_F(LiveClient, FormingUsesTheProvenCutoffInsideAFixedChunkExtent) {
    auto data = tail(0, 3, 0, 1)->columns;
    data.endMs = base + kHourMs; // bounding extent proves no future minutes
    const SparseColumns *ptr = &data;
    ComposeOptions forming; forming.forming = true;
    const auto out = compose(std::span(&ptr, 1), 5 * kMinuteMs, forming);
    EXPECT_EQ(bucketState(out, base), BucketState::Present);
    ASSERT_EQ(out.columns.size(), 1u);
    EXPECT_EQ(out.columns.front().observedMs, uint64_t(3 * kMinuteMs + 1000));
    EXPECT_EQ(bucketState(out, minute(5)), BucketState::NotLoaded);
    EXPECT_TRUE(compose(std::span(&ptr, 1), 5 * kMinuteMs).columns.empty());
}
TEST_F(LiveClient, FormingDoesNotTurnInteriorHoleIntoZero) {
    auto all = tail(0, 3, 0, 1)->columns;
    all.columns.erase(all.columns.begin() + 1);
    all.scannedRanges = {{minute(0), minute(1)}, {minute(2), minute(4)}};
    const SparseColumns *ptr = &all;
    ComposeOptions forming; forming.forming = true;
    const auto out = compose(std::span(&ptr, 1), 5 * kMinuteMs, forming);
    EXPECT_EQ(bucketState(out, base), BucketState::NotLoaded);
    EXPECT_TRUE(out.columns.empty());
}
TEST_F(LiveClient, FiveMinuteCommitBeforeOpenPublicationKeepsTheKnownPrefix) {
    cutoff = minute(3); transport.push(available(cutoff)); drainLive();
    auto &c = chart(5 * kMinuteMs); settle(); upload(c); settle();
    send(tail(3, 3, 3, 1, kMinuteMs)); settle();
    ASSERT_EQ(bucketState(*live(c).columns, base), BucketState::Present);
    cutoff = minute(4); ++chunkRevision;
    auto final = std::make_shared<ChunkFrame>(*tail(3, 3, 4, 2, kMinuteMs));
    final->columns.endMs = minute(5); // inferred open has not published a column yet
    send(final);
    now += 1000; c.pollLive(); runJobs(); // final before stored revision
    EXPECT_EQ(bucketState(*live(c).columns, base), BucketState::Present);
    ASSERT_EQ(live(c).columns->columns.size(), 1u);
    EXPECT_EQ(live(c).columns->columns.front().observedMs, uint64_t(4 * kMinuteMs));
    answer(); advance(c); // now the same prefix comes entirely from the chunk
    EXPECT_EQ(bucketState(*live(c).columns, base), BucketState::Present);
    EXPECT_EQ(live(c).columns->columns.front().observedMs, uint64_t(4 * kMinuteMs));
    send(tail(4, 4, 4, 3)); advance(c);
    EXPECT_EQ(live(c).columns->columns.front().observedMs, uint64_t(4 * kMinuteMs + 1000));
}
TEST_F(LiveClient, LiveEdgeBoundsRetainedFinalsAndDroppedMinutesAreNotLoaded) {
    ChunkStore local;
    LiveEdge edge(symbol, source);
    for (int n = 0; n <= 180; ++n) {
        auto final = std::make_shared<ChunkFrame>(*tail(n, n, n + 1, n + 1, kMinuteMs));
        final->key.startMs = hourOf(minute(n + 1));
        final->columns.endMs = minute(n + 2);
        ASSERT_TRUE(edge.accept(final, local));
        EXPECT_LE(edge.snapshot()->minutes.size(), size_t(LiveEdge::kRetainedMinutes));
    }
    const auto state = edge.snapshot();
    ASSERT_FALSE(state->minutes.empty());
    EXPECT_GE(state->minutes.begin()->first, state->openEndMs - LiveEdge::kRetainedMinutes * kMinuteMs);
    LiveComposer composer;
    const auto out = composer.compose(*state, {}, kMinuteMs, base).columns;
    EXPECT_EQ(bucketState(out, base), BucketState::NotLoaded);
    EXPECT_EQ(bucketState(out, minute(180)), BucketState::Present);
}
TEST_F(LiveClient, UnobservedOpenDoesNotConcealAMissingFinalBeforeCutoff) {
    ChunkStore local;
    LiveEdge edge(symbol, source);
    auto stored = put(local, chunk({symbol, source, kMinuteMs, base}, minute(2)));
    auto final = std::make_shared<ChunkFrame>(*tail(2, 2, 4, 1, kMinuteMs));
    final->columns.endMs = minute(5); // minute 3 final was never received; minute 4 has not published
    ASSERT_TRUE(edge.accept(final, local));
    LiveComposer composer;
    auto out = composer.compose(*edge.snapshot(), {stored}, 5 * kMinuteMs, base).columns;
    EXPECT_EQ(bucketState(out, base), BucketState::NotLoaded);
    stored = put(local, chunk({symbol, source, kMinuteMs, base}, minute(4), 2));
    out = composer.compose(*edge.snapshot(), {stored}, 5 * kMinuteMs, base).columns;
    EXPECT_EQ(bucketState(out, base), BucketState::Present);
}
TEST_F(LiveClient, OmittedProvisionalLosesItsValueIncludingAtTheEndOfAFormingPrefix) {
    for (const int missing : {1, 3}) {
        SCOPED_TRACE(missing);
        ChunkStore local;
        LiveEdge edge(symbol, source);
        ASSERT_TRUE(edge.accept(tail(0, 3, 0, 1), local));
        auto trimmed = std::make_shared<ChunkFrame>(*tail(0, 3, 0, 2));
        trimmed->columns.columns.erase(trimmed->columns.columns.begin() + missing);
        trimmed->columns.scannedRanges = {{base, minute(missing)}};
        if (missing < 3) trimmed->columns.scannedRanges.push_back({minute(missing + 1), minute(4)});
        ASSERT_TRUE(edge.accept(trimmed, local));
        EXPECT_FALSE(edge.snapshot()->minutes.contains(minute(missing)));
        LiveComposer composer;
        auto out = composer.compose(*edge.snapshot(), {}, kMinuteMs, base).columns;
        EXPECT_EQ(bucketState(out, minute(missing)), BucketState::NotLoaded);
        out = composer.compose(*edge.snapshot(), {}, 5 * kMinuteMs, base).columns;
        EXPECT_EQ(bucketState(out, base), BucketState::NotLoaded); // cannot hide the missing suffix
        auto stored = put(local, chunk({symbol, source, kMinuteMs, base}, minute(4)));
        edge.trim(local);
        out = composer.compose(*edge.snapshot(), {stored}, 5 * kMinuteMs, base).columns;
        EXPECT_EQ(bucketState(out, base), BucketState::Present);
        EXPECT_TRUE(edge.snapshot()->missingMinutes.empty());
    }
}
TEST_F(LiveClient, PublishedLiveRolloverUnionNeverShrinksAndLWaitsForUpload) {
    auto &c = chart(); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1, 1000)); settle();
    ASSERT_TRUE(c.latestLive());
    EXPECT_EQ(live(c).startMs, minute(10));
    const auto spanBeforeLiveUpdate = c.latestSnapshot();
    send(tail(10, 10, 10, 2, 2000)); advance(c);
    EXPECT_EQ(c.latestSnapshot(), spanBeforeLiveUpdate); // live revisions don't re-index spans
    std::set<int64_t> previouslyCovered;
    auto checkUnion = [&] {
        std::set<int64_t> covered;
        for (const auto &span : c.latestSnapshot()->spans)
            for (const auto &s : span.sources) if (s.build)
                for (const auto &column : s.build->resolution.columns)
                    for (const auto &r : column.sources) if (r.state == BucketState::Present) covered.insert(column.startMs);
        for (const auto &s : c.latestLive()->sources)
            for (const auto &column : s.columns->columns) covered.insert(column.bucketStartMs);
        for (const auto t : previouslyCovered) EXPECT_TRUE(covered.contains(t)) << t;
        previouslyCovered = std::move(covered);
    };
    checkUnion();
    // The final appears before disk revision. Also covers the ~1 s rollover
    // frame whose newest provisional is complete rather than kPartial.
    send(tail(10, 10, 10, 3, kMinuteMs)); advance(c); checkUnion();
    EXPECT_EQ(live(c).columns->columns.back().observedMs, uint64_t(kMinuteMs));
    cutoff = minute(11); ++chunkRevision;
    send(tail(10, 11, 11, 4)); answer();
    EXPECT_EQ(live(c).startMs, minute(10));
    runJobs(); advance(c); checkUnion();
    // Span build is published, but the node could still draw the older build.
    EXPECT_EQ(live(c).startMs, minute(10));
    EXPECT_EQ(bucketState(*live(c).columns, minute(10)), BucketState::Present);
    upload(c); advance(c); checkUnion();
    EXPECT_EQ(live(c).startMs, minute(11));
    EXPECT_EQ(bucketState(*live(c).columns, minute(10)), BucketState::NotLoaded);
}
TEST_F(LiveClient, DroppedDrawnSpanReleasesTheLiveBridgeEvenIfItRemainsPrefetch) {
    auto &c = chart(kMinuteMs, 90); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1)); settle();
    cutoff = minute(70); ++chunkRevision;
    send(tail(70, 70, 70, 2)); settle(); advance(c);
    ASSERT_EQ(live(c).startMs, minute(10)); // older visible build still needs the bridge
    c.setView(symbol, kMinuteMs, minute(64), minute(90)); settle(); advance(c);
    EXPECT_EQ(live(c).startMs, minute(70));
    EXPECT_EQ(live(c).columns->columns.size(), 1u);
    EXPECT_LE(c.stats().liveUploadedSpans, 1u);
    c.setView(symbol, kMinuteMs, minute(65), minute(90)); settle(); advance(c);
    EXPECT_EQ(live(c).startMs, minute(70)); // a later reconcile cannot revive the old E
}
TEST_F(LiveClient, MissingDrawnSpanReleasesLBeforeItsRebuildOrUpload) {
    auto &c = chart(); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1)); settle();
    const auto missing = uploadKeys(c);
    ASSERT_EQ(missing.size(), 1u);
    cutoff = minute(11); ++chunkRevision;
    send(tail(10, 11, 11, 2)); // leave chunk request and replacement span build pending
    now = 1000;
    c.capacity()->report(300ull << 20, {}, false, missing); c.pollCapacity(); drainLive();
    ASSERT_FALSE(jobs.empty());
    auto liveJob = std::move(jobs.front()); jobs.pop_front(); liveJob(); drainLive();
    ASSERT_FALSE(jobs.empty()); // replacement span has not run, much less uploaded
    EXPECT_EQ(live(c).startMs, minute(11));
    EXPECT_EQ(c.stats().liveUploadedSpans, 0u);
    settle(); advance(c);
    EXPECT_EQ(live(c).startMs, minute(11));
}
TEST_F(LiveClient, LiveWindowLagIsCappedByTwoHoursOrSixtyFourBuckets) {
    for (const int tfMinutes : {1, 5}) {
        SCOPED_TRACE(tfMinutes);
        charts.clear(); jobs.clear(); drainLive();
        cutoff = minute(10); ++chunkRevision; transport.push(available(cutoff)); drainLive();
        auto &c = chart(tfMinutes * kMinuteMs, 300); settle(); upload(c); settle();
        send(tail(10, 10, 10, 1)); settle();
        ASSERT_EQ(live(c).startMs, minute(10));
        const int open = tfMinutes == 1 ? 75 : 135;
        cutoff = minute(open); ++chunkRevision;
        send(tail(open, open, open, 2)); settle(); advance(c);
        EXPECT_EQ(live(c).startMs, minute(open)); // even a still-visible old span cannot pin forever
        EXPECT_EQ(live(c).columns->columns.size(), 1u);
    }
}
TEST_F(LiveClient, StalledCommitAndSlowerSourceCannotPinAnUnboundedCommonWindow) {
    transport.push(available(cutoff, true)); drainLive();
    auto &c = chart(kMinuteMs, 200); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1)); send(tail(10, 10, 10, 1, 1000, "hmc2.near")); settle(); advance(c);
    ASSERT_EQ(c.latestLive()->sources.size(), 2u);
    send(tail(145, 145, 10, 2)); // no commit progress, and no new frame from near
    settle(); advance(c);
    ASSERT_EQ(c.latestLive()->sources.size(), 1u); // near has no live data inside the bounded window
    EXPECT_EQ(c.latestLive()->sources.front().source, source);
    for (const auto &s : c.latestLive()->sources) {
        EXPECT_EQ(s.startMs, minute(145));
        EXPECT_LE(s.columns->columns.size(), 1u);
    }
}
TEST_F(LiveClient, UploadedAcknowledgementsStayInsideTheCurrentLiveWindow) {
    auto &c = chart(kMinuteMs, 300); settle(); upload(c); settle();
    uint64_t revision = 0;
    for (const int open : {10, 70, 130, 190, 250}) {
        cutoff = minute(open); ++chunkRevision;
        send(tail(open, open, open, ++revision)); settle(); advance(c);
        upload(c); settle(); advance(c);
        EXPECT_EQ(live(c).startMs, minute(open));
        EXPECT_LE(c.stats().liveUploadedSpans, 1u);
    }
}
TEST_F(LiveClient, PanningAcrossLiveBoundaryKeepsSubscriptionForThreeSeconds) {
    auto &a = chart(); auto &b = chart(5 * kMinuteMs); settle();
    auto leave = [&](HeatmapSourceController &c, int64_t tf) { c.setView(symbol, tf, base, minute(5)); drainLive(); };
    leave(a, kMinuteMs); leave(b, 5 * kMinuteMs);
    now = 2999; a.pollLive(); b.pollLive(); drainLive();
    EXPECT_TRUE(transport.liveUnsubscribes.empty());
    a.setView(symbol, kMinuteMs, base, minute(80)); drainLive();
    now = 3000; b.pollLive(); drainLive();
    EXPECT_TRUE(transport.liveUnsubscribes.empty()); // one chart returned before the grace period
    EXPECT_EQ(transport.liveRequests.size(), 1u);
    leave(a, kMinuteMs);
    now = 5999; a.pollLive(); drainLive(); EXPECT_TRUE(transport.liveUnsubscribes.empty());
    now = 6000; a.pollLive(); drainLive(); ASSERT_EQ(transport.liveUnsubscribes.size(), 1u);
    a.setView(symbol, kMinuteMs, base, minute(80)); drainLive();
    EXPECT_EQ(transport.liveRequests.size(), 2u);
}
TEST_F(LiveClient, LiveLatestWinsRateAndMergedAutoSummary) {
    auto &c = chart(5 * kMinuteMs); settle(); upload(c); settle();
    send(tail(10, 11, 10, 1, 1000)); // queue live job; hold it while newer data arrives
    ASSERT_FALSE(jobs.empty());
    send(tail(10, 11, 10, 2, 2000)); runJobs(); settle();
    ASSERT_TRUE(c.latestLive());
    EXPECT_EQ(live(c).revision, 1u); // publish consistent captured input, coalesce newer input
    EXPECT_EQ(c.stats().liveStaleResults, 0u);
    const auto publication = c.latestLive();
    const auto count = c.stats().liveComposedBuckets;
    send(tail(10, 11, 10, 3, 3000)); runJobs();
    EXPECT_EQ(c.latestLive(), publication); // same clock tick, no second publication
    now += HeatmapSourceController::kLiveMinIntervalMs - 1; c.pollLive(); runJobs();
    EXPECT_EQ(c.latestLive(), publication);
    now += 1; c.pollLive(); runJobs();
    EXPECT_EQ(live(c).revision, 3u);
    EXPECT_EQ(c.stats().liveComposedBuckets - count, 1u);
    const auto summary = c.latestResolution();
    ASSERT_TRUE(summary);
    const auto it = std::find_if(summary->columns.begin(), summary->columns.end(), [](const auto &r) {
        return r.startMs == minute(10);
    });
    ASSERT_NE(it, summary->columns.end());
    EXPECT_TRUE(std::any_of(it->sources.begin(), it->sources.end(), [](const auto &s) { return s.state == BucketState::Present; }));
    EXPECT_TRUE(std::any_of(summary->columns.begin(), summary->columns.end(), [](const auto &r) { return r.startMs == base; }));
}
TEST_F(LiveClient, UnrelatedHistoryEventsDoNotDiscardLiveJobsOrBypassCadenceAndBackoff) {
    for (const int costMs : {0, 6}) {
        SCOPED_TRACE(costMs);
        charts.clear(); jobs.clear(); drainLive();
        int64_t workerClock = 0;
        composeClock = [&] { const auto t = workerClock; workerClock += costMs * 1'000'000; return t; };
        now = 0; cutoff = minute(130); ++chunkRevision;
        transport.push(available(cutoff)); drainLive();
        auto &c = chart(kMinuteMs, 150); settle(); upload(c); settle();
        send(tail(130, 130, 130, 1));
        ASSERT_EQ(jobs.size(), 1u);
        auto heldLiveJob = std::move(jobs.front()); jobs.pop_front();
        auto historyBurst = [&] {
            for (int i = 0; i < 3; ++i) {
                put(store, chunk({symbol, source, kMinuteMs, base}, cutoff, ++chunkRevision));
                drainLive(); runJobs();
                auto keys = uploadKeys(c);
                std::erase_if(keys, [](const auto &k) { return k.span.startMs() >= minute(64); });
                c.capacity()->report(300ull << 20, keys); c.pollCapacity(); drainLive();
            }
        };
        historyBurst();
        send(tail(130, 130, 130, 2, 2000));
        now = HeatmapSourceController::kLiveMinIntervalMs / 2; heldLiveJob(); drainLive(); runJobs(); // before it is due
        ASSERT_TRUE(c.latestLive());
        EXPECT_EQ(live(c).revision, 1u);
        EXPECT_EQ(c.stats().liveStaleResults, 0u);
        EXPECT_EQ(c.stats().livePublications, 1u);
        const auto first = c.latestLive();
        const int interval = costMs > 5 ? HeatmapSourceController::kLiveBackoffIntervalMs
                                        : HeatmapSourceController::kLiveMinIntervalMs;
        EXPECT_EQ(c.stats().liveIntervalMs, interval);
        historyBurst();
        now = interval - 1; c.pollLive(); runJobs(); EXPECT_EQ(c.latestLive(), first);
        now = interval; c.pollLive(); runJobs(); EXPECT_EQ(live(c).revision, 2u);
        EXPECT_EQ(c.stats().livePublications, 2u); // due from job start, not completion
        historyBurst();
        now += interval; c.pollLive(); runJobs();
        EXPECT_EQ(c.stats().livePublications, 2u); // no live input changed
    }
}
TEST_F(LiveClient, WarmLiveFrameDoesNotReconcileSpansAndAutoHasUniqueSources) {
    auto &c = chart(); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1)); settle();
    const auto reconciles = c.stats().reconciles;
    send(tail(10, 10, 10, 2, 2000)); advance(c);
    EXPECT_EQ(c.stats().reconciles, reconciles);
    cutoff = minute(11); ++chunkRevision;
    send(tail(10, 11, 11, 3)); settle(); advance(c);
    EXPECT_EQ(live(c).startMs, minute(10)); // history/live overlap until upload
    for (const auto &column : c.latestResolution()->columns) {
        std::set<std::string> names;
        for (const auto &s : column.sources) EXPECT_TRUE(names.insert(s.source).second) << column.startMs;
    }
    c.setView(symbol, 5 * kMinuteMs, base, minute(80));
    ASSERT_TRUE(c.latestResolution()); // reset keeps historical Auto available
    EXPECT_FALSE(c.latestResolution()->columns.empty());
    settle();
}
TEST_F(LiveClient, MultiSourceLUsesTheLowestUploadedEndAndReconnectUsesMinimumCutoff) {
    transport.push(available(cutoff, true)); drainLive();
    auto &c = chart(); settle(); upload(c); settle();
    ASSERT_EQ(transport.liveRequests.size(), 1u);
    ASSERT_EQ(transport.liveRequests.back().sources.size(), 2u);
    send(tail(10, 10, 10, 1));
    send(tail(10, 10, 10, 1, 1000, "hmc2.near")); settle(); advance(c);
    ASSERT_EQ(c.latestLive()->sources.size(), 2u);
    cutoff = minute(11); ++chunkRevision;
    send(tail(10, 11, 11, 2));
    send(tail(10, 11, 11, 2, 1000, "hmc2.near")); settle(); advance(c);
    auto keys = uploadKeys(c);
    std::erase_if(keys, [](const auto &key) { return key.source != source; });
    c.capacity()->report(300ull << 20, keys); c.pollCapacity(); advance(c);
    for (const auto &s : c.latestLive()->sources) EXPECT_EQ(s.startMs, minute(10));
    upload(c); advance(c);
    for (const auto &s : c.latestLive()->sources) EXPECT_EQ(s.startMs, minute(11));
    // Only one source's chunk advances, so since_ms uses the slower one.
    put(store, chunk({symbol, source, kMinuteMs, base}, minute(12), 3)); drainLive();
    transport.goOffline(); transport.goOnline(); transport.push(available(minute(12), true)); drainLive();
    EXPECT_EQ(transport.liveRequests.back().sinceMs, minute(11));
}
TEST_F(LiveClient, HourBoundaryLostFinalAndDelayedUploadKeepBothMinutesCovered) {
    cutoff = minute(59); ++chunkRevision; transport.push(available(cutoff)); drainLive();
    auto &c = chart(); settle(); upload(c); settle();
    send(tail(59, 59, 59, 1, kMinuteMs)); settle();
    ASSERT_EQ(live(c).startMs, minute(59));
    cutoff = minute(60); ++chunkRevision;
    // No final 59 in this frame; it stays unknown until the chunk arrives.
    send(tail(60, 60, 60, 2)); settle(); advance(c);
    EXPECT_EQ(live(c).startMs, minute(59));
    EXPECT_EQ(bucketState(*live(c).columns, minute(59)), BucketState::Present);
    EXPECT_EQ(bucketState(*live(c).columns, minute(60)), BucketState::Present);
    EXPECT_FALSE(fetcher.live(symbol).front()->minutes.contains(minute(59))); // now supplied from sealed chunk
    upload(c); advance(c);
    EXPECT_EQ(live(c).startMs, minute(60));
}
TEST_F(LiveClient, FourHourRolloverKeepsPreviousBucketUntilUpload) {
    coarseRolloverKeepsPreviousBucket(240);
}
TEST_F(LiveClient, DailyRolloverKeepsPreviousBucketUntilUpload) {
    coarseRolloverKeepsPreviousBucket(1440);
}
TEST_F(LiveClient, CoarseTimeframesKeepOneHzAndDoNotFetchFromTheBeginningOfTheTile) {
    for (const int tfMinutes : {60, 240, 1440}) {
        SCOPED_TRACE(tfMinutes);
        charts.clear(); jobs.clear(); drainLive();
        cutoff = minute(tfMinutes - 3); ++chunkRevision;
        transport.push(available(cutoff)); drainLive();
        auto &c = chart(tfMinutes * kMinuteMs, tfMinutes * 2); settle(); upload(c); settle();
        send(tail(tfMinutes - 3, tfMinutes - 2, tfMinutes - 3, 1, 1000)); settle();
        ASSERT_TRUE(c.latestLive());
        EXPECT_EQ(live(c).startMs, base);
        EXPECT_EQ(c.stats().liveIntervalMs, HeatmapSourceController::kLiveMinIntervalMs);
        const auto count = c.stats().liveComposedBuckets, pieces = c.stats().liveCommittedPieces;
        const auto span = c.latestSnapshot();
        send(tail(tfMinutes - 3, tfMinutes - 2, tfMinutes - 3, 2, 2000)); advance(c);
        EXPECT_EQ(live(c).revision, 2u);
        EXPECT_EQ(c.stats().liveComposedBuckets - count, 1u);
        EXPECT_EQ(c.stats().liveCommittedPieces, pieces);
        EXPECT_EQ(c.latestSnapshot(), span);
        for (const auto &r : transport.requests) for (auto start : r.starts) EXPECT_GE(start, base);
    }
}
TEST_F(LiveClient, TimeframeChangeDropsOldPoolResultAndChartDestructionIsSafe) {
    auto &c = chart(); settle();
    send(tail(10, 10, 10, 1)); ASSERT_FALSE(jobs.empty());
    c.setView(symbol, 5 * kMinuteMs, base, minute(80)); drainLive();
    settle();
    ASSERT_TRUE(c.latestLive()); EXPECT_EQ(c.latestLive()->tfMs, 5 * kMinuteMs);
    EXPECT_GT(c.stats().liveStaleResults, 0u);
    now += 1000; send(tail(10, 10, 10, 2, 2000)); c.pollLive();
    ASSERT_FALSE(jobs.empty());
    charts.clear(); runJobs();
    EXPECT_EQ(cache->stats().jobs, 0u);
    EXPECT_FALSE(transport.liveUnsubscribes.empty());
}
// S5L-c review 5a: a live frame composes when it arrives (after the coalescing
// window that lets its sibling frames in), not on a fixed 1 s phase it drifts
// through; composition is still spaced by at least kLiveMinIntervalMs.
TEST_F(LiveClient, LiveFramesComposeOnArrivalAfterTheCoalescingWindow) {
    coalesceMs = 15;
    auto &c = chart(); settle(); upload(c); settle();
    send(tail(10, 10, 10, 1)); settle();
    EXPECT_FALSE(c.latestLive()) << "inside the coalescing window";
    now += 15; c.pollLive(); settle();
    ASSERT_TRUE(c.latestLive());
    EXPECT_EQ(live(c).revision, 1u);
    // 700 ms later, between two would-be 1 s deadlines: composes 15 ms after arrival.
    now += 685; send(tail(10, 10, 10, 2, 2000)); settle();
    EXPECT_EQ(live(c).revision, 1u);
    now += 15; c.pollLive(); settle();
    EXPECT_EQ(live(c).revision, 2u) << "composed on arrival, not on the next 1 s deadline";
    // A frame inside the minimum spacing waits for it (admitted at +715 ms).
    now += 100; send(tail(10, 10, 10, 3, 3000));
    now += 15; c.pollLive(); settle();
    EXPECT_EQ(live(c).revision, 2u);
    now = 715 + HeatmapSourceController::kLiveMinIntervalMs; c.pollLive(); settle();
    EXPECT_EQ(live(c).revision, 3u);
}
// Review re-check: the coalescing window is anchored to the first pending frame.
// A continuous stream (a frame every 10 ms for 2 s) still composes about every
// kLiveMinIntervalMs, and never more often.
TEST_F(LiveClient, AContinuousFrameStreamStillComposesEveryMinimumInterval) {
    coalesceMs = 15;
    auto &c = chart(); settle(); upload(c); settle();
    const auto before = c.stats().livePublications;
    for (int i = 0; i < 200; ++i) {
        now += 10;
        send(tail(10, 10, 10, uint64_t(i + 1), 1000 + uint64_t(i) * 250));
        c.pollLive(); settle();
    }
    const auto composed = c.stats().livePublications - before;
    EXPECT_GE(composed, 4u) << "a stream of frames must not postpone composition";
    EXPECT_LE(composed, 5u) << "at most one composition per minimum interval";
}
TEST_F(LiveClient, MeasuredCostAboveFiveMsBacksOffToFiveSeconds) {
    int64_t clock = 0;
    composeClock = [&] { const auto t = clock; clock += 6'000'000; return t; };
    auto &c = chart(); settle();
    send(tail(10, 10, 10, 1)); settle();
    ASSERT_TRUE(c.latestLive());
    EXPECT_EQ(c.stats().liveIntervalMs, 5000);
    EXPECT_DOUBLE_EQ(c.stats().liveComposeMs, 6);
    const auto first = c.latestLive();
    send(tail(10, 10, 10, 2, 2000));
    advance(c, 4999); EXPECT_EQ(c.latestLive(), first);
    advance(c, 1); EXPECT_EQ(live(c).revision, 2u);
}
// S5L-c review 5b: the cost that switches live composition to 5 s is the
// worker's CPU time, not wall time: a descheduled worker on a busy host (the
// live run measured single 15-58 ms updates whose median was 2-3 ms) costs
// nothing while it waits.
TEST(LiveComposeCost, TheDefaultClockIsTheWorkersCpuTime) {
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    ASSERT_TRUE(HeatmapSourceController::threadCpuClockAvailable()) << "every supported platform has a thread CPU clock";
#endif
    if (!HeatmapSourceController::threadCpuClockAvailable())
        GTEST_SKIP() << "no thread CPU clock on this platform: the backoff measures wall time (steady clock)";
    const int64_t idle0 = HeatmapSourceController::threadCpuNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int64_t idle = HeatmapSourceController::threadCpuNs() - idle0;
    EXPECT_LT(idle, 5'000'000) << "sleeping is not cost";
    const int64_t busy0 = HeatmapSourceController::threadCpuNs();
    // 50 ms of spinning: at least 20 ms even where CPU time is charged per
    // scheduler tick (Windows, about 15.6 ms).
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until) spin = spin + 1;
    EXPECT_GE(HeatmapSourceController::threadCpuNs() - busy0, 20'000'000) << "running is";
}
// ...and one slow update among fast ones does not back off: the decision is the
// median of the last three updates; two slow ones in three do.
TEST_F(LiveClient, OneSlowLiveComposeDoesNotBackOffTwoInThreeDo) {
    int64_t clock = 0, cost = 1'000'000;
    composeClock = [&] { const auto t = clock; clock += cost; return t; };
    auto &c = chart(); settle();
    send(tail(10, 10, 10, 1)); settle();
    ASSERT_TRUE(c.latestLive());
    uint64_t revision = 1;
    for (const int ms : {1, 15, 1}) {
        cost = int64_t(ms) * 1'000'000;
        now += HeatmapSourceController::kLiveMinIntervalMs;
        send(tail(10, 10, 10, ++revision, 1000 + 1000 * revision)); settle();
        EXPECT_EQ(live(c).revision, revision);
        EXPECT_EQ(c.stats().liveIntervalMs, HeatmapSourceController::kLiveMinIntervalMs) << "after a " << ms << " ms update";
    }
    cost = 15'000'000;
    now += HeatmapSourceController::kLiveMinIntervalMs;
    send(tail(10, 10, 10, ++revision, 1000 + 1000 * revision)); settle();
    EXPECT_EQ(c.stats().liveIntervalMs, HeatmapSourceController::kLiveBackoffIntervalMs) << "15, 1, 15 ms: median 15";
}
// ...and the cost is the composition (owner decision 4: "composing measures
// above 5 ms"), not the GPU image and summary built after it.
TEST_F(LiveClient, TheBackoffMeasuresCompositionNotTheImageBuiltAfterIt) {
    // Per update the clock is read at its start, around the composition, and at
    // its end: 1 ms composing, 9 ms building the image and summary.
    int64_t clock = 0;
    int call = 0;
    composeClock = [&] {
        static constexpr int64_t steps[] = {0, 1'000'000, 9'000'000, 0};
        const auto t = clock;
        clock += steps[call++ % 4];
        return t;
    };
    auto &c = chart(); settle();
    send(tail(10, 10, 10, 1)); settle();
    ASSERT_TRUE(c.latestLive());
    EXPECT_DOUBLE_EQ(c.stats().liveComposeMs, 1);
    EXPECT_DOUBLE_EQ(c.stats().liveUpdateMs, 10);
    EXPECT_EQ(c.stats().liveIntervalMs, HeatmapSourceController::kLiveMinIntervalMs);
}
TEST_F(LiveClient, RecorderReplayMatchesLiveBuilderAndAnalyticalTwapAtOneAndFiveMinutes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    recording::LiveCache mailbox;
    int64_t localNow = base;
    recording::RecorderConfig config{dir.path().toStdString(), 100, {}, {{"deep", 100, 0.5, 2, false}}, 2000, 10000};
    config.publisher = [&](recording::RecordPtr record) { mailbox.publish(std::move(record)); };
    recording::BookRecorder recorder(config, [&] { return localNow; });
    recorder.onSnapshot(symbol, base, {{true, 99, 2}, {false, 101, 4}}); recorder.drainForTest();
    localNow = base + 30000;
    recorder.onUpdates(symbol, localNow, {{true, 99, 6}, {false, 101, 8}}); recorder.drainForTest();
    localNow = base + 90000;
    recorder.onTick(localNow); recorder.drainForTest();
    // 1m committed: bid (2*30s + 6*30s)/60s = 4. Current
    // minute: bid 6 over 30s. Five-minute forming mean = 14/3.
    const auto snapshot = mailbox.snapshot(symbol, "deep");
    ASSERT_EQ(snapshot.committedThroughMs, minute(1));
    recording::RawTailBuilder raw;
    const auto bytes = raw.build(symbol, source, snapshot, base).bytes;
    ASSERT_TRUE(bytes);
    const auto frame = std::make_shared<ChunkFrame>(decodeChunk(*bytes));
    fetcher.wantLive(42, symbol);
    send(frame);
    recording::Hmc2Reader reader(dir.path().toStdString());
    for (const auto tf : {kMinuteMs, 5 * kMinuteMs}) {
        LiveComposer composer;
        const auto actual = composer.compose(*fetcher.live(symbol).front(), {}, tf, base).columns;
        recording::LiveBuilder legacy({symbol, "deep", tf, {90, 1, 20}, 1});
        const auto expected = legacy.build(reader, snapshot);
        ASSERT_EQ(expected.status, recording::BuildStatus::Complete);
        ASSERT_FALSE(actual.columns.empty());
        const auto &last = actual.columns.back();
        const auto match = std::find_if(expected.columns.begin(), expected.columns.end(), [&](const auto &c) {
            return c.bucketStartMs == last.bucketStartMs;
        });
        ASSERT_NE(match, expected.columns.end());
        EXPECT_EQ(last.observedMs, match->observedMs);
        const auto bins = binColumn(last, 90, 110, 1);
        ASSERT_EQ(bins.size(), match->cells.size());
        for (size_t i = 0; i < bins.size(); ++i) {
            EXPECT_EQ(bins[i].code, match->cells[i]) << "tf=" << tf << " row=" << i;
            EXPECT_EQ(bins[i].valid, (match->validity[i / 8] & (1u << (i % 8))) != 0);
        }
        const auto bid = binCell(last, 99, 1);
        EXPECT_NEAR(bid.bid, tf == kMinuteMs ? 6 : 14.0 / 3, .01);
    }
}
} // namespace
