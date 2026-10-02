#include "HeatmapNodeFixtures.hpp"
#include "render/heatmap/HeatmapCellQuery.hpp"
#include "heatmap/DrawPieces.hpp"
#include "heatmap/BinCell.hpp"
#include <gtest/gtest.h>
#include "../servermodel/FakeChunkTransport.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include <QCoreApplication>
#include <QEvent>
#include "SyntheticHmc2Fixture.hpp"
#include "heatmap/RecordingLoader.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QtEndian>
#include <cstring>

namespace {
using namespace heatmap;
constexpr auto start = (nodefx::kEpoch / kHourMs) * kHourMs;
constexpr auto end = start + 2 * kHourMs;
struct Fixture {
    ChunkStore store;
    std::map<std::string, std::vector<std::shared_ptr<const StoredChunk>>> chunks;
    Fixture() {
        nodefx::Shape shape{99800, 100200, 99900, 100100, 100000, true};
        for (const auto& source : {nodefx::kCoarse, nodefx::kFine}) {
            for (auto h = start; h < end; h += kHourMs) {
                auto raw = nodefx::minuteColumns(source, h, h + kHourMs, 1, shape);
                const auto coarseGrid = nodefx::minuteColumns(nodefx::kCoarse, h, h + kHourMs, 1, shape);
                for (auto& column : raw.columns) if (column.bucketStartMs >= start + 40 * kMinuteMs) {
                    for (auto& n : column.native) ++n.grid.configHash;
                    if (source == nodefx::kFine && column.bucketStartMs < start + kHourMs) {
                        const auto it = std::find_if(coarseGrid.columns.begin(), coarseGrid.columns.end(), [&](const auto& c) {
                            return c.bucketStartMs == column.bucketStartMs;
                        });
                        if (it != coarseGrid.columns.end()) column.native = it->native; // physical $1 -> $5 grid
                    }
                }
                if (h == start) {
                    raw.scannedRanges = {{h, h + 50 * kMinuteMs}, {h + 55 * kMinuteMs, h + kHourMs}};
                    std::erase_if(raw.columns, [&](const auto& c) {
                        return c.bucketStartMs >= h + 50 * kMinuteMs && c.bucketStartMs < h + 55 * kMinuteMs;
                    });
                }
                const ChunkKey key{"BTC-USD", source, kMinuteMs, h};
                chunks[source].push_back(store.put(key, std::make_shared<const SparseColumns>(raw), {true, h + kHourMs, 1}, h));
            }
        }
    }
    SparseColumns composed(const std::string& source, int64_t tf, const ComposeOptions& clip = {}) {
        std::vector<const SparseColumns*> inputs;
        for (const auto& c : chunks[source]) inputs.push_back(c->columns.get());
        return compose(inputs, tf, clip);
    }
    SpanSet spans(int64_t tf) {
        SpanSet set; set.tfMs = tf; set.symbol = "BTC-USD"; set.version = 1;
        set.availableStartMs = start; set.availableEndMs = end;
        const auto range = tiles::tilesCovering(start, end, tf);
        for (auto t = range.first; t < range.end; ++t) {
            SpanSnapshot span{{"BTC-USD", tf, t}, {}, {}, true};
            for (const auto& source : {nodefx::kCoarse, nodefx::kFine}) {
                auto b = std::make_shared<SpanSourceBuild>();
                b->key = {span.id, source, start, end, 100, 0, {}};
                b->completeEndMs = end;
                for (const auto& c : chunks[source]) b->key.generations.push_back({"BTC-USD", source, kMinuteMs,
                    c->key.startMs, c->generation, true});
                span.sources.push_back({source, b}); // upload images have been released
            }
            set.spans.push_back(std::move(span));
        }
        return set;
    }
};

TEST(HeatmapCellQuery, PriceClippedComposePreservesWordsValuesAndCoverageAtEveryTickAndTimeframe) {
    Fixture f;
    for (const auto tf : {kMinuteMs, 5 * kMinuteMs, kHourMs}) {
        for (const auto& source : {nodefx::kCoarse, nodefx::kFine}) {
            const auto full = f.composed(source, tf);
            ComposeOptions options; options.price = ComposeOptions::PriceClip{99980, 100040};
            const auto clipped = f.composed(source, tf, options);
            EXPECT_EQ(full.scannedRanges, clipped.scannedRanges);
            EXPECT_LT(sparseBytes(clipped), sparseBytes(full)); // fails if clipping is ignored
            for (const auto& c : clipped.columns) for (const auto& n : c.native)
                for (const auto& e : n.entries) {
                    const double p = (n.baseRow + e.row()) * n.grid.rowTickUnits / n.grid.priceScale;
                    EXPECT_GE(p, 99980); EXPECT_LT(p, 100040);
                }
            for (const auto tick : {1., 5., 10.}) {
                ASSERT_EQ(full.columns.size(), clipped.columns.size());
                for (size_t i = 0; i < full.columns.size(); ++i) {
                    const auto a = binColumn(full.columns[i], 99980, 100040, tick);
                    const auto b = binColumn(clipped.columns[i], 99980, 100040, tick);
                    ASSERT_EQ(a.size(), b.size());
                    for (size_t j = 0; j < a.size(); ++j) {
                        EXPECT_EQ(a[j].code, b[j].code); EXPECT_EQ(a[j].valid, b[j].valid);
                        EXPECT_DOUBLE_EQ(a[j].bid, b[j].bid); EXPECT_DOUBLE_EQ(a[j].ask, b[j].ask);
                    }
                }
            }
        }
    }
}

TEST(HeatmapCellQuery, LabelsEqualTheFillOracleAndExactValuesAfterUploadImagesAreReleased) {
    Fixture f;
    for (const auto tf : {kMinuteMs, 5 * kMinuteMs, kHourMs}) {
        auto spans = f.spans(tf);
        for (const int64_t tick : {100, 500, 1000}) {
            LabelWindowBuilder builder;
            LabelRequest q{1, 1, 0, tf, tick, 100, start / tf, 99980 * 100 / tick,
                           uint32_t((end-start) / tf), uint32_t(60 * 100 / tick), "BTC"};
            const auto labels = builder.build(q, spans, nullptr, f.store);
            auto expected = tiles::buildCells(f.composed(nodefx::kCoarse, tf), labels->grid, {start, end});
            tiles::fillVeiled(expected, tiles::buildCells(f.composed(nodefx::kFine, tf), labels->grid, {start, end}));
            ASSERT_EQ(labels->cells.size(), expected.size());
            std::vector<double> exact(expected.size());
            const auto coarse = f.composed(nodefx::kCoarse, tf), fine = f.composed(nodefx::kFine, tf);
            for (uint32_t x = 0; x < q.columns; ++x) {
                const auto time = (q.firstBucket + x) * tf;
                auto values = [&](const SparseColumns& source) {
                    const auto it = std::find_if(source.columns.begin(), source.columns.end(), [&](const auto& c) { return c.bucketStartMs == time; });
                    return it == source.columns.end() ? std::vector<BinCell>(q.rows) : binColumn(*it, 99980, 100040, tick/100.0);
                };
                const auto a = values(coarse), b = values(fine);
                for (uint32_t y = 0; y < q.rows; ++y) {
                    const auto& winner = a[y].valid ? a[y] : b[y];
                    exact[size_t(y)*q.columns+x] = winner.dominantAsk ? winner.ask : winner.bid;
                }
            }
            size_t nonzero = 0;
            for (size_t i = 0; i < expected.size(); ++i) {
                const auto& c = labels->cells[i];
                EXPECT_EQ(c.word, expected[i]) << "tf=" << tf << " tick=" << tick << " cell=" << i;
                if (tiles::cellState(c.word) == tiles::kCellValid) {
                    EXPECT_DOUBLE_EQ(c.value, exact[i]);
                    EXPECT_EQ(recording::encodeSize(c.value), c.word & 0x7fff);
                    if (c.value > 0) { ++nonzero; EXPECT_EQ(c.usd[0], '$'); EXPECT_EQ(std::string(c.asset.data()).find(' '), std::string::npos); } // number only
                } else EXPECT_EQ(c.usd[0], 0);
            }
            EXPECT_GT(nonzero, 0u);
            auto warm = builder.build(q, spans, nullptr, f.store);
            EXPECT_EQ(warm->composedWindows, 0u); EXPECT_GT(warm->cacheHits, 0u);
        }
    }
}

TEST(HeatmapCellQuery, CoarseValidCellsStopFineCompositionAndGenerationMismatchCannotLabelANewerPicture) {
    Fixture f;
    auto spans = f.spans(5 * kMinuteMs);
    LabelWindowBuilder builder;
    LabelRequest q{1, 1, 0, 5 * kMinuteMs, 1000, 100, (start + kHourMs) / (5*kMinuteMs), 9998, 12, 6, "BTC"};
    auto labels = builder.build(q, spans, nullptr, f.store);
    EXPECT_EQ(labels->composedWindows, 1u); // no veiled rows in the complete second hour
    builder.clear();
    const auto& chunk = f.chunks[nodefx::kCoarse].back();
    auto revised = std::make_shared<SparseColumns>(*chunk->columns);
    revised->columns.front().native.front().entries.front().code += 1;
    f.store.put(chunk->key, revised, {true, end, 2}, 987654);
    labels = builder.build(q, spans, nullptr, f.store);
    EXPECT_FALSE(labels->missing.empty());
    for (const auto& c : labels->cells) EXPECT_EQ(c.usd[0], 0);
}

TEST(HeatmapDrawPieces, CompleteHistoryAndLiveHaveExclusiveOwnershipAndKeepTheELHole) {
    constexpr int64_t tf = kMinuteMs, tile = start / (tiles::kTileColumns * tf);
    const auto ts = tiles::tileStartMs(tile, tf), te = tiles::tileEndMs(tile, tf);
    const std::vector<DrawSpan> spans{{11, tile, tf, ts + 20*tf}, {12, tile+1, tf, te + 40*tf}};
    const std::vector<DrawLive> live{{13, tf, ts + 25*tf, te + 10*tf}};
    const auto pieces = drawPieces(spans, live, tf);
    const std::vector<DrawPiece> expected{{13, ts+25*tf, te, true}, {11, ts, ts+20*tf, false}, {12, te, te+40*tf, false}};
    EXPECT_EQ(pieces, expected); // E < L gap and a later history span stopping live
    const std::vector<DrawSpan> complete{{11, tile, tf, te}, {12, tile+1, tf, te+64*tf}};
    const auto covered = drawPieces(complete, live, tf);
    ASSERT_EQ(covered.size(), 2u);
    EXPECT_FALSE(covered.front().live);
}

void drainQueries() {
    for (int i = 0; i < 32; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
void queryApp() {
    if (QCoreApplication::instance()) return;
    static int argc = 1;
    static char name[] = "cell-query-tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
}
TEST(HeatmapCellQuery, LatestWinsBoundedJobsPublishImmutableLabelsAndChargeTheLedger) {
    queryApp(); Fixture f;
    FakeChunkTransport transport;
    ChunkFetcher fetcher(f.store, transport);
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options;
    options.maxJobs = 1;
    options.executor = [&](auto job, int rank) { EXPECT_EQ(rank, SpanRank(SpanTier::Label, 0).fetchPriority()); jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options);
    HeatmapCellQuery query(f.store, fetcher, cache, 91);
    const auto spans = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    LabelRequest q{1, 1, 0, kMinuteMs, 100, 100, start / kMinuteMs, 99980, 10, 60, "BTC"};
    int labelSignals = 0;
    QObject::connect(&query, &HeatmapCellQuery::labelsChanged, &query, [&] { ++labelSignals; }, Qt::QueuedConnection);
    query.requestLabels(q, spans);
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_GT(cache.committedCpuBytes(), 64ull << 20);
    EXPECT_GT(f.store.stats().wantedBytes, 0u);
    ++q.serial; query.requestLabels(q, spans);
    ++q.serial; query.requestLabels(q, spans);
    EXPECT_EQ(jobs.size(), 1u);
    auto run = [&] { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); };
    run();
    EXPECT_EQ(query.latestLabels(), nullptr); EXPECT_EQ(labelSignals, 0);
    ASSERT_EQ(jobs.size(), 1u);
    run();
    const auto published = query.latestLabels();
    ASSERT_NE(published, nullptr); EXPECT_EQ(published->key.serial, 3u); EXPECT_EQ(labelSignals, 1);
    EXPECT_EQ(f.store.stats().wantedBytes, 0u);
    EXPECT_GE(cache.committedCpuBytes(), published->cells.size() * sizeof(LabelCell));
    auto old = q; old.serial = 2; query.requestLabels(old, spans);
    EXPECT_TRUE(jobs.empty()); // late delivery of an older serial cannot win
    const auto retainedCharge = cache.committedCpuBytes();
    auto invalid = q; invalid.serial = ++q.serial; invalid.firstBin = -1;
    query.requestLabels(invalid, spans);
    ASSERT_EQ(jobs.size(), 1u); run();
    EXPECT_EQ(query.latestLabels(), published);
    EXPECT_EQ(cache.committedCpuBytes(), retainedCharge); // cache AND published labels survive the error
    ++q.serial; query.requestLabels(q, spans);
    ASSERT_EQ(jobs.size(), 1u);
    query.cancel(); run();
    EXPECT_EQ(query.latestLabels(), nullptr);
    EXPECT_EQ(published->key.serial, 3u); // no mutation of published results
    EXPECT_EQ(cache.committedCpuBytes(), 0u);
}
TEST(HeatmapCellQuery, CpuRefusalDoesNotWantChunksOrSubmitJobs) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    int submitted = 0;
    SpanSourceCache::Options options; options.cpuCeiling = 1;
    options.executor = [&](auto, int) { ++submitted; };
    SpanSourceCache cache(options);
    HeatmapCellQuery query(f.store, fetcher, cache, 92);
    const auto spans = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    query.requestLabels({1, 1, 0, kMinuteMs, 100, 100, start/kMinuteMs, 99980, 10, 60, "BTC"}, spans);
    drainQueries();
    EXPECT_EQ(submitted, 0); EXPECT_TRUE(transport.requests.empty());
    EXPECT_EQ(f.store.stats().wantedBytes, 0u); EXPECT_EQ(cache.committedCpuBytes(), 0u);
}
TEST(HeatmapCellQuery, EvictedSealedChunksAreRewantedAtTheLabelRankAndReloadTheSameGeneration) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options; options.executor = [&](auto job, int) { jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options);
    HeatmapCellQuery query(f.store, fetcher, cache, 93);
    const auto spans = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    f.store.setMaxBytes(1); // bodies gone, generation identity and test copies retained
    transport.goOnline(); transport.push(nodefx::availability(start, end)); drainQueries();
    query.requestLabels({1, 1, 0, kMinuteMs, 100, 100, start/kMinuteMs, 99980, 10, 60, "BTC"}, spans);
    ASSERT_EQ(jobs.size(), 1u);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
    ASSERT_EQ(query.latestLabels(), nullptr);
    EXPECT_GT(cache.committedCpuBytes(), 0u);
    emit fetcher.chunkFailed({"OTHER-USD", nodefx::kCoarse, kMinuteMs, start}, "missing", "unrelated chart");
    drainQueries();
    EXPECT_GT(cache.committedCpuBytes(), 0u); // another chart's failure cannot cancel our reload
    std::optional<heatmap_window::WallsSnapshot> wallResult;
    WallScanRequest wall{{}, kMinuteMs, 100, 100, double(start), double(start + 10*kMinuteMs), 99980, 100040};
    query.scanWalls(wall, spans, {}, &query, [&](auto result) { wallResult = std::move(result); });
    ASSERT_EQ(jobs.size(), 1u);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
    ASSERT_TRUE(wallResult); EXPECT_EQ(wallResult->status, 200);
    ASSERT_EQ(jobs.size(), 1u); // the waiting label must resume after a wall scan
    for (int step = 0; step < 10 && !query.latestLabels(); ++step) {
        while (!jobs.empty()) { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
        fetcher.pump();
        auto requests = std::move(transport.requests); transport.requests.clear();
        for (const auto& request : requests) for (size_t i = 0; i < request.starts.size(); ++i) {
            const auto key = request.key(i);
            const auto& copies = f.chunks[key.source];
            const auto it = std::find_if(copies.begin(), copies.end(), [&](const auto& c) { return c->key == key; });
            ASSERT_NE(it, copies.end());
            auto frame = std::make_shared<ChunkFrame>(); frame->key = key;
            frame->state = {true, key.startMs + kHourMs, 1}; frame->contentHash = (*it)->contentHash;
            frame->columns = *(*it)->columns;
            transport.reply(request.id, frame);
        }
        drainQueries();
    }
    ASSERT_NE(query.latestLabels(), nullptr);
    EXPECT_TRUE(query.latestLabels()->missing.empty());
    EXPECT_GT(query.latestLabels()->cells.front().value, 0);
}
TEST(HeatmapCellQuery, HeldLabelGenerationSurvivesARevisionBeforeTheWorkerRuns) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options; options.executor = [&](auto job, int) { jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options); HeatmapCellQuery query(f.store, fetcher, cache, 96);
    const auto picture = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    query.requestLabels({1, 1, 0, kMinuteMs, 100, 100, start/kMinuteMs, 99980, 10, 60, "BTC"}, picture);
    ASSERT_EQ(jobs.size(), 1u);
    const auto old = f.chunks[nodefx::kCoarse].front();
    const auto newer = f.store.put(old->key, old->columns, {true, start+kHourMs, 2}, old->contentHash+1);
    ASSERT_NE(newer->generation, old->generation);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
    const auto labels = query.latestLabels();
    ASSERT_NE(labels, nullptr);
    EXPECT_TRUE(labels->missing.empty());
    EXPECT_EQ(labels->key.serial, 1u);
    EXPECT_TRUE(jobs.empty());
}
TEST(HeatmapCellQuery, SupersededReloadReleasesWantsAndHintsWithoutAnotherRequest) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options; options.executor = [&](auto job, int) { jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options); HeatmapCellQuery query(f.store, fetcher, cache, 94);
    const auto spans = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    f.store.setMaxBytes(1);
    query.requestLabels({1, 1, 0, kMinuteMs, 100, 100, start/kMinuteMs, 99980, 10, 60, "BTC"}, spans);
    ASSERT_EQ(jobs.size(), 1u);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
    ASSERT_GT(cache.committedCpuBytes(), 0u); // waiting, with hint bytes admitted
    const auto old = f.chunks[nodefx::kCoarse].front();
    const auto newer = f.store.put(old->key, old->columns, {true, start+kHourMs, 2}, old->contentHash+1);
    ASSERT_NE(newer->generation, old->generation);
    EXPECT_GT(f.store.stats().wantedBytes, 0u);
    emit fetcher.chunkStored(old->key, newer->generation);
    drainQueries();
    EXPECT_EQ(cache.committedCpuBytes(), 0u);
    EXPECT_EQ(f.store.stats().wantedBytes, 0u);
    EXPECT_FALSE(f.store.contains(old->key)); // release let the one-byte LRU evict it
    EXPECT_EQ(query.latestLabels(), nullptr);
    EXPECT_TRUE(jobs.empty());
}
TEST(HeatmapWalls, HeldChunksSurviveLruEvictionBeforeTheWorkerScans) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options; options.executor = [&](auto job, int) { jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options); HeatmapCellQuery query(f.store, fetcher, cache, 95);
    const auto picture = std::make_shared<const SpanSet>(f.spans(kMinuteMs));
    WallScanRequest q{{}, kMinuteMs, 1000, 100, double(start+kHourMs), double(end), 99990, 100010};
    std::optional<heatmap_window::WallsSnapshot> answer;
    query.scanWalls(q, picture, {}, &query, [&](auto result) { answer = std::move(result); });
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_EQ(f.store.stats().wantedBytes, 0u); // walls never pin via fetch wants
    f.store.setMaxBytes(1); // another builder may cause this eviction while the job is queued
    ASSERT_EQ(f.store.stats().entries, 0u);
    { auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries(); }
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->status, 200);
    EXPECT_EQ(answer->recordedColumns, 60);
    EXPECT_EQ(answer->missingColumns, 0);
    EXPECT_FALSE(answer->unknownRows);
    EXPECT_FALSE(answer->walls.empty());
    EXPECT_TRUE(transport.requests.empty());
}
TEST(HeatmapCellQuery, LabelsUseTheSharedLiveClipAtEveryTimeframeAndTick) {
    Fixture f;
    for (const auto tf : {kMinuteMs, 5*kMinuteMs, kHourMs}) for (const int64_t tick : {100, 500, 1000}) {
        auto spans = f.spans(tf);
        const auto boundary = start + tf;
        for (auto& span : spans.spans) for (auto& source : span.sources) {
            auto b = std::make_shared<SpanSourceBuild>(*source.build); b->completeEndMs = boundary; source.build = b;
        }
        LiveSnapshot live; live.tfMs = tf; live.symbol = "BTC-USD"; live.version = 7;
        for (const auto& source : {nodefx::kCoarse, nodefx::kFine}) {
            auto columns = std::make_shared<SparseColumns>(f.composed(source, tf));
            for (auto& c : columns->columns) for (auto& n : c.native) for (auto& numerator : n.numerators) numerator *= 7;
            LiveSourceSnapshot s; s.source = source; s.startMs = start; s.openEndMs = end; s.columns = columns;
            live.sources.push_back(std::move(s));
        }
        LabelRequest q{1, 1, 7, tf, tick, 100, start/tf, 99980*100/tick, uint32_t((end-start)/tf), uint32_t(60*100/tick), "BTC"};
        LabelWindowBuilder builder; auto labels = builder.build(q, spans, &live, f.store);
        auto history = tiles::buildCells(f.composed(nodefx::kCoarse, tf), labels->grid, {start, end});
        tiles::fillVeiled(history, tiles::buildCells(f.composed(nodefx::kFine, tf), labels->grid, {start, end}));
        auto forming = tiles::buildCells(*live.sources[0].columns, labels->grid, {start, end});
        tiles::fillVeiled(forming, tiles::buildCells(*live.sources[1].columns, labels->grid, {start, end}));
        for (uint32_t y = 0; y < q.rows; ++y) for (uint32_t x = 0; x < q.columns; ++x) {
            const auto i = size_t(y)*q.columns+x;
            EXPECT_EQ(labels->cells[i].word, x == 0 ? history[i] : forming[i]) << tf << ":" << tick << ":" << x;
            if (tiles::cellState(labels->cells[i].word) == tiles::kCellValid)
                EXPECT_EQ(recording::encodeSize(labels->cells[i].value), labels->cells[i].word & 0x7fff);
        }
        EXPECT_TRUE(labels->formingColumns.back());
        ++live.version; ++q.liveVersion; ++q.serial;
        auto update = builder.build(q, spans, &live, f.store);
        EXPECT_GT(update->reusedHistoryColumns, 0u);
        EXPECT_EQ(update->composedWindows, 0u);
        EXPECT_EQ(update->cacheHits, 0u); // only the live window ran on this revision
        WallScanRequest wall{{}, tf, tick, 100, double(end-tf), double(end), 99980, 100040};
        wall.query.priceMin = 99980; wall.query.priceMax = 100040;
        auto walls = scanWalls(wall, spans, &live, f.store, builder);
        ASSERT_FALSE(walls.walls.empty());
        for (const auto& w : walls.walls) EXPECT_TRUE(w.forming);
        // The E/L hole stays loading; a later live start cannot backfill it.
        for (auto& source : live.sources) source.startMs = boundary + tf;
        ++live.version; ++q.liveVersion; ++q.serial;
        auto gap = builder.build(q, spans, &live, f.store);
        for (uint32_t y = 0; y < q.rows; ++y)
            EXPECT_EQ(tiles::cellState(gap->cells[size_t(y)*q.columns+1].word), tiles::kCellLoading);
    }
}
TEST(HeatmapWalls, ViewportMarginExplicitPeriodTickBudgetAndMissingColumns) {
    Fixture f; const auto spans = f.spans(kMinuteMs); LabelWindowBuilder builder;
    WallScanRequest q{{}, kMinuteMs, 500, 100, double(start+kHourMs), double(end), 99990, 100010};
    auto walls = scanWalls(q, spans, nullptr, f.store, builder);
    EXPECT_EQ(walls.status, 200); EXPECT_TRUE(walls.gpuRenderer);
    EXPECT_EQ(walls.rangeStartMs, start+kHourMs); EXPECT_EQ(walls.rangeEndMs, end);
    EXPECT_EQ(walls.rangePriceMin, 99970); EXPECT_EQ(walls.rangePriceMax, 100030); EXPECT_EQ(walls.bandTick, 5);
    ASSERT_FALSE(walls.walls.empty());
    EXPECT_GE(walls.walls.front().qty, walls.walls.back().qty);
    q.query.startMs = start; q.query.endMs = start + 60*kMinuteMs; q.query.tick = 1;
    walls = scanWalls(q, spans, nullptr, f.store, builder);
    EXPECT_EQ(walls.status, 200); EXPECT_EQ(walls.bandTick, 1); EXPECT_EQ(walls.missingColumns, 5);
    q.query.tick = 1.234;
    const auto badTick = scanWalls(q, spans, nullptr, f.store, builder);
    EXPECT_EQ(badTick.status, 422);
    EXPECT_EQ(badTick.error, heatmap_window::WallError::BadTick);
    q.query.tick = .01; q.query.endMs = start + 1000000*kMinuteMs;
    EXPECT_EQ(scanWalls(q, spans, nullptr, f.store, builder).status, 422);
}

recording::Hmc2Record parityHour(const std::vector<recording::Hmc2Record>& minutes) {
    auto out = minutes.front();
    out.header.tfMs = kHourMs;
    out.bucketStartMs = out.bucketStartMs / kHourMs * kHourMs;
    out.entries.clear(); out.coverage.clear(); out.observedMs = 0; out.flags = 0;
    out.bidRowLo = out.askRowLo = INT64_MAX;
    out.bidRowHi = out.askRowHi = 0;
    for (const auto& r : minutes) {
        out.observedMs += r.observedMs;
        out.flags |= r.flags;
        out.bidRowLo = std::min(out.bidRowLo, r.bidRowLo);
        out.bidRowHi = std::max(out.bidRowHi, r.bidRowHi);
        out.askRowLo = std::min(out.askRowLo, r.askRowLo);
        out.askRowHi = std::max(out.askRowHi, r.askRowHi);
    }
    for (bool ask : {false, true}) {
        for (int64_t row = ask ? out.askRowLo : out.bidRowLo; row <= (ask ? out.askRowHi : out.bidRowHi); ++row) {
            uint32_t covered = 0;
            long double numerator = 0;
            for (const auto& r : minutes) {
                if (row < (ask ? r.askRowLo : r.bidRowLo) || row > (ask ? r.askRowHi : r.bidRowHi)) continue;
                covered += r.observedMs;
                for (const auto& e : r.entries)
                    if (e.row == row && e.isAsk == ask)
                        numerator += static_cast<long double>(recording::decodeSize(e.twapCode, r.header.sizeScale)) * r.observedMs;
            }
            if (!covered) continue;
            if (!out.coverage.empty() && out.coverage.back().isAsk == ask &&
                out.coverage.back().hi + 1 == row && out.coverage.back().coveredMs == covered)
                out.coverage.back().hi = row;
            else out.coverage.push_back({row, row, ask, covered});
            if (numerator) {
                const auto code = recording::encodeSize(static_cast<double>(numerator / covered), out.header.sizeScale);
                out.entries.push_back({row, ask, code, code, covered});
            }
        }
    }
    std::sort(out.entries.begin(), out.entries.end(), [](const auto& a, const auto& b) {
        return std::pair(a.row, a.isAsk) < std::pair(b.row, b.isAsk);
    });
    return out;
}
TEST(HeatmapWalls, MatchesLegacyCaptureAtItsBandTickOnTheSameHmc2Recording) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    constexpr auto epoch = synthetic_hmc2::epoch;
    {
        recording::Hmc2Store writer(dir.path().toStdString());
        std::array<std::vector<recording::Hmc2Record>, 2> hours;
        for (int64_t i = 0; i < 120; ++i) for (const auto* layer : {"deep", "near"}) {
            if (i == 7 || i == 53) continue;
            auto record = synthetic_hmc2::syntheticMinute(layer, i);
            if (i >= 35) ++record.header.configHash;
            writer.append(record);
            if (std::string(layer) == "deep") hours[size_t(i/60)].push_back(record);
        }
        for (const auto& minutes : hours) writer.append(parityHour(minutes));
    }
    recording::Hmc2Reader reader(dir.path().toStdString());
    ChunkStore store;
    std::map<std::string, std::vector<std::shared_ptr<const StoredChunk>>> chunks;
    for (const auto& source : kChunkSources) for (int h = 0; h < 2; ++h) {
        const auto from = epoch + h*kHourMs;
        const ChunkKey key{"BTC-USD", std::string(source.id), kMinuteMs, from};
        auto raw = loadRecording(reader, "BTC-USD", std::string(source.hmc2Layer), kMinuteMs, from, from+kHourMs);
        chunks[std::string(source.id)].push_back(store.put(key, std::make_shared<const SparseColumns>(raw), {true, from+kHourMs, 1}, from));
    }
    for (const auto tf : {kMinuteMs, 5*kMinuteMs, kHourMs}) {
        if (tf == kHourMs) {
            auto hours = loadRecording(reader, "BTC-USD", "deep", kHourMs, epoch, epoch+2*kHourMs);
            const ChunkKey key{"BTC-USD", nodefx::kCoarse, kHourMs, epoch};
            chunks[nodefx::kCoarse] = {store.put(key, std::make_shared<const SparseColumns>(hours),
                                                {false, epoch+2*kHourMs, 1}, 555)};
        }
        recording::BuildRequest q{"BTC-USD", tf, epoch+2*kHourMs-tf, uint32_t(2*kHourMs/tf), 100000, 100200, 20, 10.};
        const auto page = recording::buildPage(reader, q);
        ASSERT_EQ(page.status, recording::BuildStatus::Complete) << page.message;
        ASSERT_FALSE(page.columns.empty()); ASSERT_EQ(page.band.tick, 10);
        heatmap_window::ColumnWindow legacy;
        legacy.configure(tf, int(q.count), int(q.count));
        heatmap_window::Update update;
        const double low = page.band.lo, high = low + page.band.tick*page.band.rows;
        legacy.setDisplayBand({low, high, page.band.tick}, 1, update); legacy.setRecordingRequest("parity");
        std::vector<heatmap_window::Column> columns;
        double quantization = 0;
        for (const auto& c : page.columns) {
            heatmap_window::Column out;
            out.bucketStartMs = c.bucketStartMs; out.minPrice = low; out.maxPrice = high; out.tickSize = page.band.tick;
            out.liquidityScale = c.quantityScale; out.observedMs = c.observedMs;
            out.intensity.resize(int(c.cells.size()*2)); out.liquidity.resize(int(c.quantities.size()*2));
            for (size_t i = 0; i < c.cells.size(); ++i) {
                const auto code = qToLittleEndian(c.cells[i]), qty = qToLittleEndian(c.quantities[i]);
                std::memcpy(out.intensity.data()+i*2, &code, 2); std::memcpy(out.liquidity.data()+i*2, &qty, 2);
            }
            out.validity = QByteArray(reinterpret_cast<const char*>(c.validity.data()), qsizetype(c.validity.size()));
            quantization = std::max(quantization, c.quantityScale);
            columns.push_back(out);
        }
        bool first = false;
        ASSERT_TRUE(legacy.ingestRecording(columns, 1, "parity", epoch, epoch+2*kHourMs, true, epoch,
            epoch+2*kHourMs-tf, page.sizeScale.floor, page.sizeScale.codesPerOctave, update, first));
        SpanSet spans; spans.symbol = "BTC-USD"; spans.tfMs = tf; spans.version = 1;
        spans.availableStartMs = epoch; spans.availableEndMs = epoch+2*kHourMs;
        const auto range = tiles::tilesCovering(epoch, epoch+2*kHourMs, tf);
        for (auto tile = range.first; tile < range.end; ++tile) {
            SpanSnapshot span{{"BTC-USD", tf, tile}, {}, {}, true};
            for (const auto& source : kChunkSources) {
                auto b = std::make_shared<SpanSourceBuild>();
                b->key = {span.id, std::string(source.id), epoch, epoch+2*kHourMs, 100, 0, {}}; b->completeEndMs = epoch+2*kHourMs;
                for (const auto& c : chunks[std::string(source.id)]) b->key.generations.push_back({"BTC-USD", std::string(source.id), c->key.levelMs,
                    c->key.startMs, c->generation, c->sealed});
                span.sources.push_back({std::string(source.id), b});
            }
            // The wire's sources are coarse then fine in this synthetic recording.
            std::stable_sort(span.sources.begin(), span.sources.end(), [](const auto& a, const auto& b) {
                return a.source == nodefx::kCoarse && b.source != nodefx::kCoarse;
            });
            spans.spans.push_back(std::move(span));
        }
        heatmap_window::WallQuery wallQuery; wallQuery.limit = 100; wallQuery.startMs = epoch; wallQuery.endMs = epoch+2*kHourMs;
        wallQuery.priceMin = low; wallQuery.priceMax = high;
        const auto expected = legacy.captureWalls(wallQuery);
        LabelWindowBuilder builder;
        auto actual = scanWalls({wallQuery, tf, 1000, 100, double(epoch), double(epoch+2*kHourMs), low, high},
                                spans, nullptr, store, builder);
        EXPECT_EQ(actual.status, 200); EXPECT_EQ(actual.recordedColumns, expected.recordedColumns);
        EXPECT_EQ(actual.missingColumns, expected.missingColumns); ASSERT_EQ(actual.walls.size(), expected.walls.size());
        for (const auto& wall : actual.walls) {
            const auto found = std::find_if(expected.walls.begin(), expected.walls.end(), [&](const auto& e) {
                return e.priceLow == wall.priceLow && e.ask == wall.ask;
            });
            ASSERT_NE(found, expected.walls.end());
            EXPECT_NEAR(wall.qty, found->qty, quantization * .501 + 1e-12);
            EXPECT_NEAR(wall.meanQty, found->meanQty, quantization * .501 + 1e-12);
            EXPECT_NEAR(wall.notional, found->notional, quantization * wall.priceHigh * .501 + 1e-8);
            EXPECT_EQ(wall.firstSeenMs, found->firstSeenMs); EXPECT_EQ(wall.lastSeenMs, found->lastSeenMs);
            EXPECT_EQ(wall.columns, found->columns); EXPECT_EQ(wall.bucketStartMs, found->bucketStartMs);
        }
    }
}
TEST(HeatmapCellQuery, TickChangesReuseTickFreeCompositionsAndTextUsesTheCellMidpoint) {
    Fixture f; auto spans = f.spans(kMinuteMs); LabelWindowBuilder builder;
    LabelRequest q{1, 1, 0, kMinuteMs, 1000, 100, (start+kHourMs)/kMinuteMs, 9998, 12, 6, "BTC"};
    builder.build(q, spans, nullptr, f.store);
    q.tickUnits = 500; q.firstBin *= 2; q.rows *= 2; ++q.serial;
    const auto labels = builder.build(q, spans, nullptr, f.store);
    EXPECT_EQ(labels->composedWindows, 0u); EXPECT_GT(labels->cacheHits, 0u);
    auto parse = [](std::string text) {
        if (text.front() == '$') text.erase(text.begin());
        text = text.substr(0, text.find(' '));
        const char suffix = text.back();
        double scale = suffix == 'k' ? 1e3 : suffix == 'M' ? 1e6 : suffix == 'B' ? 1e9 : suffix == 'T' ? 1e12 : 1;
        return std::stod(text) * scale;
    };
    size_t checked = 0;
    for (uint32_t y = 0; y < q.rows; ++y) for (uint32_t x = 0; x < q.columns; ++x) {
        const auto& cell = labels->cells[size_t(y)*q.columns+x];
        if (!(cell.value > 0)) continue;
        ++checked;
        const double usd = cell.value * (q.firstBin+q.rows-y-.5) * 5;
        EXPECT_NEAR(parse(cell.usd.data()), usd, .501 * std::pow(10., std::floor(std::log10(usd))-2));
        EXPECT_NEAR(parse(cell.asset.data()), cell.value, .501 * std::pow(10., std::floor(std::log10(cell.value))-2));
    }
    EXPECT_GT(checked, 0u);
}
TEST(HeatmapWalls, AsyncScansNeedNoLabelsAndNeverFetchUnheldHistory) {
    queryApp(); Fixture f;
    FakeChunkTransport transport; ChunkFetcher fetcher(f.store, transport);
    transport.goOnline(); transport.push(nodefx::availability(start, end)); drainQueries();
    std::deque<std::function<void()>> jobs;
    SpanSourceCache::Options options; options.executor = [&](auto job, int) { jobs.push_back(std::move(job)); };
    SpanSourceCache cache(options); HeatmapCellQuery query(f.store, fetcher, cache, 99);
    auto picture = std::make_shared<SpanSet>(f.spans(kMinuteMs)); picture->spans.clear(); // period outside drawn spans
    WallScanRequest q{{}, kMinuteMs, 1000, 100, double(start), double(end), 99990, 100010};
    std::optional<heatmap_window::WallsSnapshot> answer;
    query.scanWalls(q, picture, {}, &query, [&](auto result) { answer = std::move(result); });
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_EQ(query.latestLabels(), nullptr); EXPECT_TRUE(transport.requests.empty());
    auto job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries();
    ASSERT_TRUE(answer); EXPECT_EQ(answer->status, 200); EXPECT_FALSE(answer->walls.empty());
    EXPECT_TRUE(transport.requests.empty());
    f.store.setMaxBytes(1); answer.reset();
    query.cancel(); // also discard its composed windows
    query.scanWalls(q, picture, {}, &query, [&](auto result) { answer = std::move(result); });
    ASSERT_EQ(jobs.size(), 1u);
    job = std::move(jobs.front()); jobs.pop_front(); job(); drainQueries();
    ASSERT_TRUE(answer); EXPECT_EQ(answer->recordedColumns, 0); EXPECT_GT(answer->missingColumns, 0);
    EXPECT_TRUE(transport.requests.empty());
}
} // namespace
