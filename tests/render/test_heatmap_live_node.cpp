// Slice S5L-c: the live edge in HeatmapTileNode (docs/research/2026-09-s5l-plan.md
// sections 3-5). GPU cases run on the selected QRhi backend (lab::HeadlessRhi /
// OffscreenQuick) and skip cleanly when it cannot create a QRhi with compute.
// Deterministic: hand-made SpanSets and LiveSnapshots (the fake controller, built
// with the production composer and GPU source builder), or the real controller on
// FakeChunkTransport with a manual build executor and an injected clock.
//
// The oracle for drawn cells is the CPU kernel twin: every minute of the
// recording composed at the timeframe with the forming bucket kept
// (TimeComposer forming), binned per source (tiles::buildCells = binColumn per
// column) and combined with the fill rule (tiles::fillVeiled). Whatever bin
// draws a bucket (span or live), its GPU cells must equal that oracle.
#include "HeatmapNodeFixtures.hpp"
#include "HeatmapNodeScene.hpp"
#include "protocol/SentinelStreamClient.hpp" // shared-frame metatype
#include "../servermodel/FakeChunkTransport.hpp"
#include "heatmap/TimeComposer.hpp"
#include "lab/RhiBackend.hpp"
#include "render/heatmap/HeatmapCellQuery.hpp"
#include "render/heatmap/HeatmapLabelMatch.hpp"
#include <QEvent>
#include <QGuiApplication>
#include <gtest/gtest.h>
#include <rhi/qrhi.h>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace {
using namespace heatmap;
using namespace heatmap::gpu;
using namespace nodefx;
using Segment = HeatmapTileStats::Segment;
constexpr int64_t minute = kMinuteMs;
const std::string kSymbol = "BTC-USD";

int64_t ceilTo(int64_t t, int64_t step) { return recording::floorDiv(t + step - 1, step) * step; }

// The open minute as the recorder publishes it: observed for `observed` ms; rows
// from `partialFromPrice` up were covered for only half of that (they veil);
// every other covered row is observed the whole time (a row without an entry is a
// real zero).
void makePartial(SparseColumn &column, uint64_t observed, int64_t partialFromPrice) {
    column.observedMs = observed;
    column.flags |= recording::kPartial | recording::kProvisional;
    for (auto &n : column.native) {
        n.observedMs = observed;
        const int64_t split = partialFromPrice * 100 / n.grid.rowTickUnits; // absolute native row
        for (auto &side : n.coverage) {
            std::vector<CoverageRun> runs;
            for (const auto &run : side) {
                if (run.lo < split) runs.push_back({run.lo, std::min(run.hi, split - 1), observed});
                if (run.hi >= split) runs.push_back({std::max(run.lo, split), run.hi, observed / 2});
            }
            side = std::move(runs);
        }
        std::erase_if(n.entries, [&](const SparseEntry &e) { return n.baseRow + int64_t(e.row()) >= split; });
    }
}

// The recording both the chunks and the live edge serve: deterministic minutes per
// source (HeatmapNodeFixtures), so a minute has the same value wherever it comes from.
struct Recording {
    Shape shape;
    int64_t partialFromPrice = 100'040; // rows of the open minute above this are partly covered
    uint64_t revision = 1;               // minute sizes (a later revision changes every value)
    // [from, open + 1 min): full minutes, then the open minute observed `observed` ms.
    SparseColumns minutes(const std::string &source, int64_t from, int64_t open, uint64_t observed) const {
        auto out = minuteColumns(source, from, open + minute, revision, shape);
        if (observed < uint64_t(minute) && !out.columns.empty() && out.columns.back().bucketStartMs == open)
            makePartial(out.columns.back(), observed, partialFromPrice);
        validate(out);
        return out;
    }
    // Every minute in [from, open + 1 min) composed at tf with the forming bucket.
    SparseColumns composed(const std::string &source, int64_t tf, int64_t from, int64_t open, uint64_t observed) const {
        const auto all = minutes(source, from, open, observed);
        ComposeOptions o;
        o.startMs = from;
        o.endMs = ceilTo(open + minute, tf);
        o.forming = true;
        const SparseColumns *level = &all;
        return compose(std::span<const SparseColumns *const>(&level, 1), tf, o);
    }
};

// Span builds of an open chunk: minutes before `cutoff` (E = the last complete
// bucket end), as the controller builds them from the stored chunks.
struct Spans {
    Recording rec;
    FakeSpans fake; // assembles SpanSets; availability set per step
    std::map<std::tuple<int64_t, int64_t, std::string, int64_t>, SpanSourceBuildPtr> builds;
    HeldChunks held; // the chunks of the builds (S7b: the label oracle reads them)
    SpanSourceBuildPtr build(int64_t tf, int64_t tile, const std::string &source, int64_t cutoff) {
        auto &slot = builds[{tf, tile, source, cutoff}];
        if (slot) return slot;
        const SpanId span{kSymbol, tf, tile};
        SpanSourceInput input;
        input.key = {span, source, std::clamp(fake.availableStartMs, span.startMs(), span.endMs()),
                     std::clamp(fake.availableEndMs, span.startMs(), span.endMs()), 100,
                     uint64_t(cutoff) * 31 + uint64_t(tile) * 7 + uint64_t(tf), {}};
        for (int64_t h = recording::floorDiv(span.startMs(), kHourMs) * kHourMs; h < span.endMs(); h += kHourMs) {
            if (h >= cutoff || h + kHourMs <= fake.availableStartMs) continue;
            const int64_t through = std::min(h + kHourMs, cutoff);
            auto chunk = std::make_shared<StoredChunk>();
            chunk->key = {kSymbol, source, kMinuteMs, h};
            auto columns = minuteColumns(source, h, through, rec.revision, rec.shape);
            columns.endMs = h + kHourMs; // the chunk's extent; scanned only through the cutoff
            chunk->columns = std::make_shared<const SparseColumns>(std::move(columns));
            chunk->sealed = through == h + kHourMs;
            chunk->committedThroughMs = through;
            chunk->generation = uint64_t(through / minute) + (rec.revision - 1) * 1'000'000'000; // new values, new generation
            chunk->contentHash = chunk->generation;
            chunk->bytes = sparseBytes(*chunk->columns);
            input.key.generations.push_back({kSymbol, source, kMinuteMs, h, chunk->generation, chunk->sealed});
            held[chunk->key] = chunk;
            input.chunks.push_back(std::move(chunk));
        }
        std::sort(input.key.generations.begin(), input.key.generations.end());
        slot = buildSpanSource(input);
        return slot;
    }
    FakeSpans::Span span(int64_t tf, int64_t tile, int64_t cutoff, SpanRank rank = {SpanTier::Visible, 0}) {
        return {tile, rank, {build(tf, tile, kCoarse, cutoff), build(tf, tile, kFine, cutoff)}, tf};
    }
    // The live window [L, open + 1 min) at tf, as the controller publishes it.
    std::shared_ptr<const LiveSnapshot> live(int64_t tf, int64_t L, int64_t open, uint64_t observed, uint64_t version,
                                             uint64_t serial = 1) const {
        auto out = std::make_shared<LiveSnapshot>();
        out->version = version;
        out->serial = serial;
        out->symbol = kSymbol;
        out->tfMs = tf;
        out->resolution.tfMs = tf;
        out->resolution.priceScale = 100;
        for (const auto *source : {&kCoarse, &kFine}) {
            LiveSourceSnapshot s;
            s.source = *source;
            s.revision = version;
            s.startMs = L;
            s.openEndMs = open + minute;
            s.columns = std::make_shared<const SparseColumns>(rec.composed(*source, tf, L, open, observed));
            s.gpu = std::make_shared<const GpuSource>(buildGpuSource(*s.columns));
            const auto summary = summarizeResolution(*s.columns, s.source, tf, L, ceilTo(open + minute, tf), 100);
            for (const auto &column : summary.columns)
                for (const auto &c : column.sources)
                    if (c.commonUnits > 0) s.commonUnits = s.commonUnits ? std::lcm(s.commonUnits, c.commonUnits) : c.commonUnits;
            mergeResolution(out->resolution, summary);
            out->sources.push_back(std::move(s));
        }
        std::sort(out->sources.begin(), out->sources.end(),
                  [](const auto &a, const auto &b) { return a.commonUnits > b.commonUnits; });
        return out;
    }
};

std::vector<Segment> layer0(const HeatmapTileStats &stats) {
    auto all = stats.segments();
    std::erase_if(all, [](const Segment &s) { return s.layer != 0; });
    return all;
}
// Every bucket of [lo, hi) is drawn by exactly one current-picture segment (a
// span bin, the live bin or the loading hatch). Returns the violations.
std::vector<std::string> bucketsNotOnce(const std::vector<Segment> &segments, int64_t tf, int64_t lo, int64_t hi) {
    std::vector<std::string> out;
    for (int64_t b = lo; b < hi; b += tf) {
        const int64_t mid = b + tf / 2;
        int n = 0;
        for (const auto &s : segments) n += s.loMs <= mid && mid < s.hiMs;
        if (n != 1) out.push_back("bucket +" + std::to_string((b - lo) / tf) + " drawn " + std::to_string(n) + "x");
    }
    return out;
}
// The end of the time the current picture draws from data (span or live) starting at lo.
int64_t dataCoverageEnd(const std::vector<Segment> &segments, int64_t lo) {
    std::vector<std::pair<int64_t, int64_t>> data;
    for (const auto &s : segments)
        if (s.kind != Segment::Loading) data.emplace_back(s.loMs, s.hiMs);
    std::sort(data.begin(), data.end());
    int64_t end = lo;
    for (const auto &[a, b] : data)
        if (a <= end) end = std::max(end, b);
    return end;
}

// Compares the cells of every bucket the current picture draws from a bin with
// the oracle (all minutes composed at tf with the forming bucket, binned per
// source, fill rule). Returns the number of buckets compared; counts mismatches.
struct OracleCheck {
    size_t buckets = 0, liveBuckets = 0, mismatches = 0;
    std::string first;
};
OracleCheck checkCells(const Recording &rec, const HeatmapCellCapture &capture, const std::vector<Segment> &segments,
                       int64_t tf, int64_t from, int64_t open, uint64_t observed) {
    OracleCheck out;
    std::map<std::string, SparseColumns> composed;
    for (const auto *source : {&kCoarse, &kFine}) composed[*source] = rec.composed(*source, tf, from, open, observed);
    for (const auto &segment : segments) {
        if (segment.kind == Segment::Loading) continue;
        for (int64_t b = std::max(ceilTo(segment.loMs, tf), from); b < segment.hiMs; b += tf) {
            const int64_t bucket = b / tf;
            for (const auto &block : capture.blocks) {
                if (block.bin != segment.bin || bucket < block.grid.firstBucket ||
                    bucket >= block.grid.firstBucket + int64_t(block.grid.columns))
                    continue;
                const auto &data = block.result->data;
                if (data.size() != qsizetype(uint64_t(block.grid.columns) * block.grid.rows * 4)) {
                    ++out.mismatches;
                    out.first = "readback size";
                    continue;
                }
                tiles::TileGrid grid;
                grid.tfMs = tf;
                grid.tile = tiles::tileOfBucket(bucket);
                grid.firstBucket = tiles::tileFirstBucket(grid.tile);
                grid.tickUnits = int64_t(std::llround(block.grid.displayTick * 100));
                grid.tick = block.grid.displayTick;
                grid.firstBin = block.grid.firstBin;
                grid.rows = block.grid.rows;
                auto oracle = tiles::buildCells(composed[kCoarse], grid, {});
                tiles::fillVeiled(oracle, tiles::buildCells(composed[kFine], grid, {}));
                const auto *cells = reinterpret_cast<const uint32_t *>(data.constData());
                const size_t gpuColumn = size_t(bucket - block.grid.firstBucket);
                const size_t oracleColumn = size_t(bucket - grid.firstBucket);
                for (uint32_t r = 0; r < grid.rows; ++r) {
                    const uint32_t g = cells[size_t(r) * block.grid.columns + gpuColumn];
                    const uint32_t o = oracle[size_t(r) * grid.columns + oracleColumn];
                    const bool bad = tiles::cellState(g) != tiles::cellState(o) ||
                                     (tiles::cellState(o) == tiles::kCellValid && (g & 0xffffu) != (o & 0xffffu));
                    if (bad && out.first.empty())
                        out.first = std::string(block.live ? "live" : "span") + " bucket " + std::to_string(bucket) +
                                    " row " + std::to_string(r) + " gpu state " + std::to_string(tiles::cellState(g)) +
                                    " oracle state " + std::to_string(tiles::cellState(o));
                    out.mismatches += bad;
                }
                ++out.buckets;
                out.liveBuckets += block.live;
            }
        }
    }
    return out;
}

class LiveNode : public testing::Test {
protected:
    Spans spans;
    std::unique_ptr<Scene> scene;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
            GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        scene = std::make_unique<Scene>();
        ASSERT_TRUE(scene->create(QSize(480, 200))) << scene->error.toStdString();
        spans.fake.availableStartMs = kEpoch;
        auto &f = scene->host->frame;
        f.tickUnits = 500; // $5: both sources build it (the fine one fills the coarse gaps)
        f.view.priceLo = 99'800;
        f.view.priceHi = 100'200;
        f.crossfadeMs = 0;
    }
    HeatmapTileNode::Frame &frame() { return scene->host->frame; }
    const HeatmapTileStats &stats() const { return *scene->host->stats; }
    bool render(int n = 1) {
        for (int i = 0; i < n; ++i)
            if (!scene->frame()) return false;
        return true;
    }
    // Shows SpanSet + LiveSnapshot: the spans of `tiles` built through `cutoff`
    // and the live window [L, open + 1 min) with the open minute observed `observed`.
    void show(int64_t tf, std::vector<int64_t> tileList, int64_t cutoff, int64_t L, int64_t open, uint64_t observed,
              uint64_t version) {
        spans.fake.availableEndMs = open + minute;
        std::vector<FakeSpans::Span> list;
        for (const int64_t t : tileList) list.push_back(spans.span(tf, t, cutoff));
        frame().tfMs = tf;
        frame().spans = spans.fake.set(tf, std::move(list));
        frame().live = version ? spans.live(tf, L, open, observed, version) : nullptr;
    }
};

// ---------------------------------------------------------------- draw clip
// E/L clip and the live oracle: the span bin draws before its complete end E,
// the live bin [max(L, E), open end), a gap [E, L) draws loading, and every
// bucket in view comes from exactly one of them. Whichever bin draws a bucket,
// its GPU cells equal binColumn over compose(all minutes, forming) with the fill
// rule, at 1m (live window across a tile boundary) and 5m, at $5 and $1.
TEST_F(LiveNode, DrawClipCoversEachBucketOnceAndMatchesTheComposedOracle) {
    struct Case {
        int64_t tf, tileOffset, cutoffBack, liveBack, openAhead;
        uint64_t observed;
        const char *what;
    };
    // T: a tile boundary. E = T - cutoffBack, L = T - liveBack, open minute at T + openAhead.
    const std::vector<Case> cases{
        {minute, 2, 3 * minute, 6 * minute, minute, 25'000, "1m, L < E (overlap), window across the tile edge"},
        {minute, 2, 3 * minute, minute, minute, 25'000, "1m, L > E (gap draws loading)"},
        {5 * minute, 1, 10 * minute, 15 * minute, 3 * minute, 40'000, "5m, forming bucket of 3.7 minutes"},
    };
    for (const auto &c : cases) {
        for (const int64_t tick : {500, 100}) {
            SCOPED_TRACE(std::string(c.what) + " tick=" + std::to_string(tick));
            const int64_t tf = c.tf, T = kEpoch + c.tileOffset * tiles::kTileColumns * tf;
            const int64_t E = T - c.cutoffBack, L = T - c.liveBack, open = T + c.openAhead;
            const int64_t tileBefore = tiles::tileOfBucket(T / tf) - 1;
            const int64_t viewLo = T - 24 * tf, viewHi = T + 8 * tf;
            frame().tickUnits = tick;
            frame().view.timeLoMs = double(viewLo);
            frame().view.timeHiMs = double(viewHi);
            show(tf, {tileBefore, tileBefore + 1}, E, L, open, c.observed, 1); // the open tile too (E = its start)
            ASSERT_TRUE(render(3));
            frame().capture = std::make_shared<HeatmapCellCapture>();
            ASSERT_TRUE(render());
            const auto capture = frame().capture;
            frame().capture.reset();
            const auto segments = layer0(stats());
            const int64_t liveEnd = ceilTo(open + minute, tf);
            // S7a: the pure clip consumed by labels equals the actual node's
            // pieces, including its timeframe-rounded live end.
            std::vector<DrawSpan> clipSpans;
            for (const auto& span : frame().spans->spans) {
                int64_t complete = INT64_MAX;
                for (const auto& source : span.sources) if (source.build)
                    complete = std::min(complete, source.build->completeEndMs);
                if (complete != INT64_MAX) clipSpans.push_back({uint64_t(span.id.tile), span.id.tile, tf, complete});
            }
            const std::vector<DrawLive> clipLive{{0, tf, L, liveEnd}};
            for (const auto& piece : drawPieces(clipSpans, clipLive, tf)) {
                if (piece.hiMs <= piece.loMs) continue;
                EXPECT_EQ(std::count_if(segments.begin(), segments.end(), [&](const auto& s) {
                    return s.kind == (piece.live ? Segment::Live : Segment::Span) &&
                           s.loMs == piece.loMs && s.hiMs == piece.hiMs;
                }), 1);
            }
            const auto notOnce = bucketsNotOnce(segments, tf, viewLo, liveEnd);
            EXPECT_TRUE(notOnce.empty()) << notOnce.size() << " buckets, first: " << (notOnce.empty() ? "" : notOnce[0]);
            // The live bin starts at max(L, E); a gap [E, L) is loading.
            int64_t liveLo = INT64_MAX, liveHi = 0, loading = 0;
            for (const auto &s : segments) {
                if (s.kind == Segment::Live) { liveLo = std::min(liveLo, s.loMs); liveHi = std::max(liveHi, s.hiMs); }
                if (s.kind == Segment::Loading && s.hiMs > viewLo && s.loMs < liveEnd) loading += s.hiMs - s.loMs;
            }
            EXPECT_EQ(liveLo, std::max(L, E));
            EXPECT_EQ(liveHi, liveEnd) << "the forming bucket is drawn by the live bin";
            EXPECT_EQ(loading, std::max<int64_t>(0, L - E)) << "only the gap [E, L) draws loading";
            EXPECT_EQ(stats().liveDrawFromMs.load(), std::max(L, E));
            const auto check = checkCells(spans.rec, *capture, segments, tf, viewLo, open, c.observed);
            EXPECT_EQ(check.mismatches, 0u) << check.first;
            EXPECT_GE(check.buckets, size_t((liveEnd - viewLo) / tf - std::max<int64_t>(0, L - E) / tf));
            EXPECT_EQ(check.liveBuckets, size_t((liveEnd - std::max(L, E)) / tf));
            EXPECT_EQ(stats().errors.load(), 0u);
        }
    }
}

// S7b (plan section 6 item 2): the label words (HeatmapCellQuery's oracle on the
// drawn picture: the same SpanSet, live snapshot, timeframe and tick) equal the
// cells the node draws, read back from the GPU, for every bucket of the current
// picture: span bins with the fine source's fill pass, the live bin with its
// forming bucket, the current picture during a crossfade, and a picture held
// across a timeframe switch (labels follow the drawn picture, not the target).
struct LabelParity {
    size_t cells = 0, liveCells = 0, valid = 0, mismatches = 0;
    std::string first;
};
LabelParity compareLabels(const LabelCells &labels, const HeatmapCellCapture &capture, const std::vector<Segment> &segments,
                          int64_t tf) {
    LabelParity out;
    const auto &k = labels.key;
    for (const auto &segment : segments) {
        if (segment.kind == Segment::Loading) continue;
        for (int64_t b = ceilTo(segment.loMs, tf); b < segment.hiMs; b += tf) {
            const int64_t bucket = b / tf;
            if (bucket < k.firstBucket || bucket >= k.firstBucket + int64_t(k.columns)) continue;
            for (const auto &block : capture.blocks) {
                if (block.bin != segment.bin || bucket < block.grid.firstBucket ||
                    bucket >= block.grid.firstBucket + int64_t(block.grid.columns))
                    continue;
                const auto *gpu = reinterpret_cast<const uint32_t *>(block.result->data.constData());
                if (block.result->data.size() != qsizetype(uint64_t(block.grid.columns) * block.grid.rows * 4)) {
                    ++out.mismatches;
                    out.first = "readback size";
                    continue;
                }
                for (uint32_t r = 0; r < block.grid.rows; ++r) {
                    const int64_t bin = block.grid.firstBin + int64_t(block.grid.rows - 1 - r); // row 0 is the top
                    if (bin < k.firstBin || bin >= k.firstBin + int64_t(k.rows)) continue;
                    const uint32_t g = gpu[size_t(r) * block.grid.columns + size_t(bucket - block.grid.firstBucket)];
                    const uint32_t l = labels.cells[size_t(k.firstBin + k.rows - 1 - bin) * k.columns +
                                                    size_t(bucket - k.firstBucket)].word;
                    const bool bad = tiles::cellState(g) != tiles::cellState(l) ||
                                     (tiles::cellState(l) == tiles::kCellValid && (g & 0xffffu) != (l & 0xffffu));
                    if (bad && out.first.empty())
                        out.first = std::string(block.live ? "live" : "span") + " bucket " + std::to_string(bucket) +
                                    " bin " + std::to_string(bin) + " gpu " + std::to_string(g) + " label " + std::to_string(l);
                    out.mismatches += bad;
                    ++out.cells;
                    out.liveCells += block.live;
                    out.valid += tiles::cellState(l) == tiles::kCellValid;
                }
            }
        }
    }
    return out;
}

TEST_F(LiveNode, LabelWordsEqualTheDrawnBins) {
    const int64_t tf = minute, T = kEpoch + 2 * tiles::kTileColumns * tf;
    const int64_t E = T - 3 * minute, L = T - 6 * minute, open = T + minute;
    const int64_t viewLo = T - 24 * tf, viewHi = T + 8 * tf, liveEnd = ceilTo(open + minute, tf);
    const int64_t tileBefore = tiles::tileOfBucket(T / tf) - 1;
    frame().view.timeLoMs = double(viewLo);
    frame().view.timeHiMs = double(viewHi);
    auto labelsFor = [&](int64_t tick, const SpanSet &set, const LiveSnapshot *live) {
        LabelRequest q;
        q.serial = 1;
        q.tfMs = set.tfMs;
        q.tickUnits = tick;
        q.priceScale = 100;
        q.firstBucket = viewLo / set.tfMs;
        q.columns = uint32_t((liveEnd - viewLo) / set.tfMs);
        q.firstBin = int64_t(std::floor(frame().view.priceLo * 100 / double(tick)));
        q.rows = uint32_t(std::ceil(frame().view.priceHi * 100 / double(tick))) - uint32_t(q.firstBin);
        q.asset = "BTC";
        ChunkStore store;
        LabelWindowBuilder builder;
        auto out = builder.build(q, set, live, store, spans.held);
        EXPECT_TRUE(out->missing.empty());
        return out;
    };
    auto capture = [&] {
        frame().capture = std::make_shared<HeatmapCellCapture>();
        EXPECT_TRUE(render());
        auto out = frame().capture;
        frame().capture.reset();
        return out;
    };
    show(tf, {tileBefore, tileBefore + 1}, E, L, open, 25'000, 1);
    // $5 (the coarse source draws, the fine one fills its veil) and $1.
    for (const int64_t tick : {500, 100}) {
        SCOPED_TRACE("tick " + std::to_string(tick));
        frame().tickUnits = tick;
        frame().crossfadeMs = 0;
        ASSERT_TRUE(render(3));
        const auto cap = capture();
        const auto parity = compareLabels(*labelsFor(tick, *frame().spans, frame().live.get()), *cap, layer0(stats()), tf);
        EXPECT_EQ(parity.mismatches, 0u) << parity.first;
        EXPECT_GT(parity.valid, 500u);
        EXPECT_GT(parity.liveCells, 0u) << "the live bin (forming bucket) is compared";
        EXPECT_GT(stats().fillPasses.load(), 0u);
    }
    // A crossfade: the current picture (layer 0, the new tick) is what labels follow.
    frame().crossfadeMs = 5000;
    frame().tickUnits = 500;
    const auto fading = capture();
    ASSERT_TRUE(stats().crossfading.load());
    ASSERT_EQ(stats().drawnTickUnits.load(), 500);
    auto parity = compareLabels(*labelsFor(500, *frame().spans, frame().live.get()), *fading, layer0(stats()), tf);
    EXPECT_EQ(parity.mismatches, 0u) << parity.first;
    EXPECT_GT(parity.valid, 500u);
    frame().crossfadeMs = 0;
    ASSERT_TRUE(render(2));
    // A hold across a timeframe switch: the 5m spans cannot upload (no budget), so
    // the 1m picture stays drawn; labels of the drawn (1m) picture equal it.
    const auto drawnSet = frame().spans;
    const auto drawnLive = frame().live;
    frame().uploadBudgetBytes = 1;
    show(5 * minute, {tiles::tileOfBucket(T / (5 * minute)) - 1, tiles::tileOfBucket(T / (5 * minute))}, E,
         L / (5 * minute) * (5 * minute), open, 25'000, 2);
    frame().live.reset(); // the 5m live window is not uploaded either
    const auto held = capture();
    ASSERT_TRUE(stats().holding.load());
    ASSERT_EQ(stats().drawnTfMs.load(), tf);
    parity = compareLabels(*labelsFor(500, *drawnSet, drawnLive.get()), *held, layer0(stats()), tf);
    EXPECT_EQ(parity.mismatches, 0u) << parity.first;
    EXPECT_GT(parity.valid, 500u);
    EXPECT_EQ(stats().errors.load(), 0u);
}

// S7b review: labels must describe the picture on screen. A new live version
// pages in over several frames (a small upload budget) while the node keeps
// drawing the previous one, and then a span revision (a later complete end) does
// the same through slot fallback. Each frame, as the chart does, the matched
// columns are computed BEFORE the frame (from the last frame's drawn segments and
// this frame's target); after it, every matched column's label words equal the
// cells the node drew in that frame, through every handover frame.
TEST_F(LiveNode, MatchedLabelColumnsEqualTheDrawnCellsThroughHandovers) {
    const int64_t tf = minute, T = kEpoch + 2 * tiles::kTileColumns * tf;
    const int64_t E = T - 3 * minute, L = T - 6 * minute, open = T + minute;
    const int64_t viewLo = T - 24 * tf, viewHi = T + 8 * tf;
    const int64_t tileBefore = tiles::tileOfBucket(T / tf) - 1;
    frame().view.timeLoMs = double(viewLo);
    frame().view.timeHiMs = double(viewHi);
    frame().tickUnits = 500;
    frame().crossfadeMs = 0;
    auto labelsFor = [&](const SpanSet &set, const LiveSnapshot *live) {
        LabelRequest q;
        q.serial = 1;
        q.tfMs = tf;
        q.tickUnits = 500;
        q.priceScale = 100;
        q.firstBucket = viewLo / tf;
        q.columns = uint32_t((viewHi + 4 * tf - viewLo) / tf);
        q.firstBin = int64_t(std::floor(frame().view.priceLo / 5));
        q.rows = uint32_t(std::ceil(frame().view.priceHi / 5)) - uint32_t(q.firstBin);
        q.liveVersion = live ? live->version : 0;
        ChunkStore store;
        LabelWindowBuilder builder;
        return builder.build(q, set, live, store, spans.held);
    };
    show(tf, {tileBefore, tileBefore + 1}, E, L, open, 25'000, 1);
    ASSERT_TRUE(render(3));
    std::vector<std::shared_ptr<const LabelCells>> labels{labelsFor(*frame().spans, frame().live.get())};
    std::vector<Segment> drawn;
    std::vector<uint8_t> ok;
    size_t compared = 0, liveCompared = 0, mismatches = 0, framesOnOldLive = 0, framesOnFallback = 0;
    std::string first;
    // One frame as the chart draws it: match first, then render and compare.
    auto frameAndCompare = [&] {
        stats().copySegments(drawn);
        std::vector<std::vector<uint8_t>> masks;
        for (const auto &l : labels) {
            matchLabelColumns(*l, drawn, frame().spans.get(), frame().live.get(), ok);
            masks.push_back(ok);
        }
        frame().capture = std::make_shared<HeatmapCellCapture>();
        EXPECT_TRUE(render());
        const auto capture = frame().capture;
        frame().capture.reset();
        const auto segments = layer0(stats());
        for (size_t i = 0; i < labels.size(); ++i) {
            const auto &k = labels[i]->key;
            for (uint32_t x = 0; x < k.columns; ++x) {
                if (!masks[i][x]) continue;
                // The cells drawn for that bucket this frame.
                const int64_t bucket = k.firstBucket + x;
                const Segment *seg = nullptr;
                for (const auto &sg : segments)
                    if (sg.kind != Segment::Loading && sg.loMs <= bucket * tf && bucket * tf < sg.hiMs) seg = &sg;
                if (!seg) { ++mismatches; if (first.empty()) first = "matched column not drawn"; continue; }
                for (const auto &block : capture->blocks) {
                    if (block.bin != seg->bin || bucket < block.grid.firstBucket ||
                        bucket >= block.grid.firstBucket + int64_t(block.grid.columns))
                        continue;
                    const auto *gpu = reinterpret_cast<const uint32_t *>(block.result->data.constData());
                    for (uint32_t r = 0; r < block.grid.rows; ++r) {
                        const int64_t bin = block.grid.firstBin + int64_t(block.grid.rows - 1 - r);
                        if (bin < k.firstBin || bin >= k.firstBin + int64_t(k.rows)) continue;
                        const uint32_t g = gpu[size_t(r) * block.grid.columns + size_t(bucket - block.grid.firstBucket)];
                        const uint32_t w = labels[i]->cells[size_t(k.firstBin + k.rows - 1 - bin) * k.columns + x].word;
                        const bool bad = tiles::cellState(g) != tiles::cellState(w) ||
                                         (tiles::cellState(w) == tiles::kCellValid && (g & 0xffffu) != (w & 0xffffu));
                        if (bad && first.empty())
                            first = "labels v" + std::to_string(k.liveVersion) + " bucket " + std::to_string(bucket) +
                                    (block.live ? " live" : " span") + " bin " + std::to_string(bin);
                        mismatches += bad;
                        ++compared;
                        liveCompared += block.live;
                    }
                }
            }
        }
    };
    // 1. A new live version: the open minute completes and the next one opens.
    frame().uploadBudgetBytes = 2048;
    frame().live = spans.live(tf, L, open + minute, 25'000, 2);
    labels.push_back(labelsFor(*frame().spans, frame().live.get()));
    for (int i = 0; i < 3000 && (stats().liveVersion.load() != 2 || i < 3); ++i) {
        frameAndCompare();
        framesOnOldLive += stats().liveVersion.load() == 1;
    }
    frameAndCompare(); // one more frame on the new version
    ASSERT_EQ(stats().liveVersion.load(), 2u) << "the new live version paged in";
    EXPECT_GT(framesOnOldLive, 0u) << "the node drew the old version while the new one paged in";
    // Labels of the new version match the live columns now; the old ones never do.
    stats().copySegments(drawn);
    EXPECT_GT(matchLabelColumns(*labels[1], drawn, frame().spans.get(), frame().live.get(), ok), 0u);
    size_t liveMatched = 0;
    for (uint32_t x = 0; x < labels[1]->key.columns; ++x) liveMatched += ok[x] && labels[1]->liveColumns[x];
    EXPECT_GT(liveMatched, 0u);
    matchLabelColumns(*labels[0], drawn, frame().spans.get(), frame().live.get(), ok);
    for (uint32_t x = 0; x < labels[0]->key.columns; ++x) EXPECT_FALSE(ok[x] && labels[0]->liveColumns[x]);
    // 2. A span revision: both tiles are rebuilt through a later complete end with
    //    new values (new generations). They upload in small steps: slot fallback
    //    (the previous values) meanwhile.
    labels.clear();
    labels.push_back(labelsFor(*frame().spans, frame().live.get())); // the picture drawn now
    spans.rec.revision = 2;
    show(tf, {tileBefore, tileBefore + 1}, E + 2 * minute, L, open + minute, 25'000, 3);
    labels.push_back(labelsFor(*frame().spans, frame().live.get()));  // the revision's
    frame().uploadBudgetBytes = 16384;
    for (int i = 0; i < 3000 && (stats().fallbackSlots.load() > 0 || i < 3); ++i) {
        frameAndCompare();
        framesOnFallback += stats().fallbackSlots.load() > 0;
    }
    frameAndCompare();
    EXPECT_GT(framesOnFallback, 0u) << "the revision drew its previous content (slot fallback) first";
    EXPECT_EQ(stats().fallbackSlots.load(), 0u);
    stats().copySegments(drawn);
    EXPECT_GT(matchLabelColumns(*labels[1], drawn, frame().spans.get(), frame().live.get(), ok), 20u);
    // 3. The same live version, a revised span (new values) whose complete end
    //    moves INTO the drawn live window: when it is ready the node draws those
    //    buckets from the span, not from the unchanged live bin.
    const auto liveBefore = frame().live;
    const uint64_t liveVersion = stats().liveVersion.load();
    labels.clear();
    labels.push_back(labelsFor(*frame().spans, frame().live.get()));
    spans.rec.revision = 3;
    {
        std::vector<FakeSpans::Span> list;
        for (const int64_t t : {tileBefore, tileBefore + 1}) list.push_back(spans.span(tf, t, T + minute));
        frame().spans = spans.fake.set(tf, std::move(list)); // the live snapshot stays
    }
    labels.push_back(labelsFor(*frame().spans, frame().live.get()));
    size_t liveLabelColumnsNowSpan = 0;
    for (uint32_t x = 0; x < labels[0]->key.columns; ++x)
        liveLabelColumnsNowSpan += labels[0]->liveColumns[x] && !labels[1]->liveColumns[x];
    ASSERT_GT(liveLabelColumnsNowSpan, 0u) << "the revision takes buckets from the live window";
    framesOnFallback = 0;
    for (int i = 0; i < 3000 && (stats().fallbackSlots.load() > 0 || i < 3); ++i) {
        frameAndCompare();
        framesOnFallback += stats().fallbackSlots.load() > 0;
    }
    frameAndCompare();
    EXPECT_GT(framesOnFallback, 0u);
    EXPECT_EQ(frame().live, liveBefore);
    EXPECT_EQ(stats().liveVersion.load(), liveVersion) << "the live version never changed";
    EXPECT_EQ(mismatches, 0u) << first;
    EXPECT_GT(compared, 5000u);
    EXPECT_GT(liveCompared, 0u);
    EXPECT_EQ(stats().errors.load(), 0u);
}

// The partial column (spec rule 5): the forming minute is the time-weighted
// average over the time covered so far. Rows covered for the whole observed time
// are observed (a row without an entry is a real zero, drawn as valid); rows
// covered for part of it draw the veil, never zero.
TEST_F(LiveNode, PartialColumnVeilsRowsNotCoveredForTheWholeObservedTime) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, open = T - 4 * minute;
    frame().tickUnits = 100; // $1: one native row of the fine source per cell
    frame().view = {double(T - 20 * minute), double(T), 99'950, 100'090};
    show(tf, {tiles::tileOfBucket(T / tf) - 1}, open, open, open, 20'000, 1);
    frame().capture = std::make_shared<HeatmapCellCapture>();
    ASSERT_TRUE(render(2));
    const auto capture = frame().capture;
    const auto segments = layer0(stats());
    const auto check = checkCells(spans.rec, *capture, segments, tf, T - 20 * minute, open, 20'000);
    EXPECT_EQ(check.mismatches, 0u) << check.first;
    ASSERT_EQ(check.liveBuckets, 1u) << "the forming minute is drawn by the live bin";
    // Read the forming column of the live bin: states by price.
    const HeatmapCellCapture::Block *live = nullptr;
    for (const auto &b : capture->blocks)
        if (b.live) live = &b;
    ASSERT_TRUE(live);
    const auto *cells = reinterpret_cast<const uint32_t *>(live->result->data.constData());
    const size_t column = size_t(open / tf - live->grid.firstBucket);
    uint32_t zeroBelow = 0, validBelow = 0, veilAbove = 0, validAbove = 0;
    for (uint32_t r = 0; r < live->grid.rows; ++r) {
        const double price = double(live->grid.firstBin + int64_t(live->grid.rows - 1 - r)) * live->grid.displayTick;
        if (price < 99'960 || price > 100'080) continue; // inside the fine band, inside the view
        const uint32_t cell = cells[size_t(r) * live->grid.columns + column];
        const auto state = tiles::cellState(cell);
        if (price + 1 <= spans.rec.partialFromPrice) {
            validBelow += state == tiles::kCellValid;
            zeroBelow += state == tiles::kCellValid && (cell & 0x7fffu) == 0;
        } else if (price >= spans.rec.partialFromPrice) {
            veilAbove += state == tiles::kCellVeil;
            validAbove += state == tiles::kCellValid;
        }
    }
    EXPECT_GT(validBelow, 50u) << "rows covered the whole observed time are drawn";
    EXPECT_GT(zeroBelow, 0u) << "a covered row without an entry is a real zero";
    EXPECT_GT(veilAbove, 20u) << "rows covered for part of the observed time veil";
    EXPECT_EQ(validAbove, 0u) << "never zero (or a value) where not observed the whole time";
}

// ---------------------------------------------------------------- rollover
// Minute rollover never blanks: a scripted sequence of fake-controller snapshots
// (forming minute grows, rolls over into the next minute and across a tile edge,
// the chunk commits and its rebuilt span uploads over several frames at a small
// budget, then L advances as the controller does after the upload report). In
// every frame the current picture draws data from the view start without a hole,
// its end never moves left, and every bucket is drawn exactly once.
TEST_F(LiveNode, RolloverSequenceNeverShrinksDrawnCoverage) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs; // tile edge
    const int64_t tile1 = tiles::tileOfBucket(T / tf) - 1, tile2 = tile1 + 1;
    const int64_t viewLo = T - 40 * minute;
    frame().view.timeLoMs = double(viewLo);
    frame().view.timeHiMs = double(T + 6 * minute);
    struct Step {
        std::vector<int64_t> tiles;
        int64_t cutoff, L, open;
        uint64_t observed;
        int frames;
        uint64_t budget;
    };
    const std::vector<Step> steps{
        {{tile1}, T - 2 * minute, T - 2 * minute, T - 2 * minute, 10'000, 3, 8u << 20},
        {{tile1}, T - 2 * minute, T - 2 * minute, T - 2 * minute, 40'000, 2, 8u << 20},
        {{tile1}, T - 2 * minute, T - 2 * minute, T - minute, 5'000, 2, 8u << 20},   // rollover: T-2 pending
        {{tile1}, T - minute, T - 2 * minute, T - minute, 15'000, 12, 64u << 10},    // commit: rebuilt span uploads slowly
        {{tile1}, T - minute, T - minute, T - minute, 30'000, 2, 8u << 20},          // upload reported: L advances
        {{tile1}, T - minute, T - minute, T, 3'000, 2, 8u << 20},                    // rollover across the tile edge
        {{tile1, tile2}, T, T - minute, T, 10'000, 12, 64u << 10},                   // commit; tile 2 appears
        {{tile1, tile2}, T, T, T, 20'000, 2, 8u << 20},                              // L advances
        {{tile1, tile2}, T, T, T + minute, 2'000, 3, 8u << 20},                      // the next rollover
    };
    uint64_t version = 0;
    int64_t lastEnd = 0;
    int frames = 0, fallbackFrames = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto &s = steps[i];
        show(tf, s.tiles, s.cutoff, s.L, s.open, s.observed, ++version);
        frame().uploadBudgetBytes = s.budget;
        for (int f = 0; f < s.frames; ++f) {
            ASSERT_TRUE(render());
            ++frames;
            const auto segments = layer0(stats());
            const int64_t end = dataCoverageEnd(segments, viewLo);
            EXPECT_GE(end, lastEnd) << "step " << i << " frame " << f << ": drawn coverage shrank";
            EXPECT_GE(end, ceilTo(s.open + minute, tf)) << "step " << i << " frame " << f << ": the forming minute is drawn";
            const auto notOnce = bucketsNotOnce(segments, tf, viewLo, ceilTo(s.open + minute, tf));
            EXPECT_TRUE(notOnce.empty()) << "step " << i << " frame " << f << ": " << notOnce.size()
                                         << " buckets, first: " << (notOnce.empty() ? "" : notOnce[0]);
            for (const auto &seg : segments)
                EXPECT_FALSE(seg.kind == Segment::Loading && seg.loMs < end && seg.hiMs > viewLo)
                    << "step " << i << " frame " << f << ": loading inside the drawn data";
            lastEnd = std::max(lastEnd, end);
            fallbackFrames += stats().fallbackSlots.load() > 0;
        }
    }
    EXPECT_GT(frames, 35);
    EXPECT_GT(fallbackFrames, 4) << "the rebuilt span uploaded over several frames (its old bin drew meanwhile)";
    EXPECT_EQ(stats().missingDraws.load(), 0u);
    EXPECT_EQ(stats().errors.load(), 0u);
}

// L advances only after every source of the live-edge span reported its upload
// (S5L-b). Where the view does not overlap a finer source's band nothing bins
// it, so the node acknowledges it without an upload (no GPU memory); once the
// view moves into the band the released image is reported missing, so the
// controller rebuilds it. Elsewhere a finer source out of its band just waits.
TEST_F(LiveNode, LiveEdgeSpansAcknowledgeOutOfBandSourcesWithoutUploadingThem) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t open = T - 4 * minute;
    frame().view = {double(T - 3 * kTileMs), double(T), 103'000, 103'400}; // far above the fine band
    auto capacity = std::make_shared<HeatmapCapacity>();
    frame().capacity = capacity;
    spans.fake.availableEndMs = open + minute;
    frame().tfMs = tf;
    auto list = [&](bool released) {
        std::vector<FakeSpans::Span> out{spans.span(tf, tile1 - 2, open), spans.span(tf, tile1 - 1, open),
                                         spans.span(tf, tile1, open)};
        if (released) // the controller released the acknowledged image
            for (auto &b : out.back().sources) b = FakeSpans::released(b);
        return out;
    };
    frame().spans = spans.fake.set(tf, list(false));
    frame().live = spans.live(tf, open, open, 20'000, 1);
    ASSERT_TRUE(render(3));
    std::set<std::pair<int64_t, std::string>> uploaded;
    for (const auto &key : capacity->take().uploaded) uploaded.emplace(key.span.tile, key.source);
    EXPECT_TRUE(uploaded.contains({tile1, kFine})) << "the live-edge span's fine source is acknowledged";
    EXPECT_TRUE(uploaded.contains({tile1, kCoarse}));
    EXPECT_FALSE(uploaded.contains({tile1 - 2, kFine})) << "away from the live edge only wanted sources report";
    EXPECT_TRUE(uploaded.contains({tile1 - 2, kCoarse}));
    EXPECT_EQ(stats().acknowledgedOnly.load(), 1u);
    EXPECT_EQ(stats().residentSources.load(), 3u) << "the acknowledged source takes no GPU memory";
    // The live bin bins both sources (one fill pass per bin); span bins only the coarse one.
    EXPECT_EQ(stats().fillPasses.load(), stats().liveBinPasses.load() / 2) << "out of its band, no span fill pass";
    // The controller released both images (it was told both are uploaded); the
    // node holds the coarse one, and reports nothing missing while out of band.
    frame().spans = spans.fake.set(tf, list(true));
    ASSERT_TRUE(render(3));
    capacity->take();
    EXPECT_EQ(stats().missingReports.load(), 0u) << "out of band, the acknowledged source is not missing";
    // Into the fine band: the acknowledged source is now wanted and reported missing (rebuild).
    frame().view.priceLo = 99'800;
    frame().view.priceHi = 100'200;
    ASSERT_TRUE(render(2));
    std::set<std::pair<int64_t, std::string>> missing;
    for (const auto &key : capacity->take().missing) missing.emplace(key.span.tile, key.source);
    EXPECT_TRUE(missing.contains({tile1, kFine})) << "wanted now: the controller must rebuild it";
    EXPECT_FALSE(missing.contains({tile1, kCoarse})) << "the coarse source is held";
}

// Review fix 1: a live replacement pages in within the frame's upload budget
// (shared with span sources) and the previous live picture keeps drawing until
// all of its sources are resident. A 45-minute bridge (about 1 MB over two
// sources) at a 64 KiB budget takes many frames, none above the budget.
TEST_F(LiveNode, LiveUploadsStayWithinTheFrameBudgetWhileThePreviousVersionDraws) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t cutoff = T - 50 * minute;
    frame().view = {double(T - 60 * minute), double(T), 99'800, 100'200};
    show(tf, {tile1 - 1, tile1}, cutoff, cutoff, cutoff, 20'000, 1);
    ASSERT_TRUE(render(3));
    ASSERT_EQ(stats().liveVersion.load(), 1u);
    constexpr uint64_t budget = 64u << 10;
    frame().uploadBudgetBytes = budget;
    const uint64_t uploadedBefore = stats().liveUploadBytes.load();
    show(tf, {tile1 - 1, tile1}, cutoff, cutoff, cutoff + 45 * minute, 30'000, 2);
    uint64_t imageBytes = 0;
    for (const auto &source : frame().live->sources) imageBytes += source.gpu->bytes();
    ASSERT_GT(imageBytes, 8 * budget);
    int frames = 0, oldFrames = 0;
    uint64_t maxFrame = 0;
    while (stats().liveVersion.load() != 2 && frames < 200) {
        ASSERT_TRUE(render());
        ++frames;
        maxFrame = std::max<uint64_t>(maxFrame, stats().frameUploadBytes.load());
        if (stats().liveVersion.load() == 1) {
            ++oldFrames;
            bool live = false;
            for (const auto &seg : layer0(stats())) live = live || (seg.kind == Segment::Live && seg.liveVersion == 1);
            EXPECT_TRUE(live) << "frame " << frames << ": the previous live picture keeps drawing";
        }
    }
    EXPECT_EQ(stats().liveVersion.load(), 2u) << "the replacement completed";
    EXPECT_LE(maxFrame, budget) << "no frame records more than its upload budget";
    EXPECT_GE(oldFrames, int(imageBytes / budget) - 1) << "it paged in over many frames";
    EXPECT_EQ(stats().liveUploadBytes.load() - uploadedBefore, imageBytes);
    EXPECT_EQ(stats().errors.load(), 0u);
}

// Review fix 3: live residency follows GPU pressure. A long bridge (L lagging)
// grows the live buffer sets; once the window is small again and the node is
// over a tight cap, the oversized spare goes and the next refill right-sizes the
// set it reuses, so resident bytes come back under the cap and the capacity
// report shows free bytes again.
TEST_F(LiveNode, LiveCapacityRecoversAfterALargeWindowShrinksUnderATightCap) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t cutoff = T - 55 * minute;
    frame().view = {double(T - 60 * minute), double(T), 99'800, 100'200};
    auto capacity = std::make_shared<HeatmapCapacity>();
    frame().capacity = capacity;
    uint64_t version = 0;
    auto small = [&](int n) {
        for (int i = 0; i < n; ++i) {
            show(tf, {tile1 - 1, tile1}, T - 2 * minute, T - 2 * minute, T - 2 * minute, 5'000 + 4'000 * i, ++version);
            ASSERT_TRUE(render(2));
        }
    };
    small(6);
    const uint64_t baseline = stats().residentBytes.load();
    for (int i = 0; i < 3; ++i) { // a 53-minute bridge
        show(tf, {tile1 - 1, tile1}, cutoff, cutoff, T - 2 * minute, 10'000 + 5'000 * i, ++version);
        ASSERT_TRUE(render(2));
    }
    const uint64_t peak = stats().residentBytes.load();
    ASSERT_GT(peak, baseline + (512u << 10)) << "the bridge grew the live sets";
    const uint64_t cap = baseline + (peak - baseline) / 4;
    frame().gpuCapBytes = cap;
    small(6);
    EXPECT_LE(stats().residentBytes.load(), cap) << "resident " << stats().residentBytes.load() << " baseline "
                                                 << baseline << " peak " << peak;
    const auto report = capacity->take();
    EXPECT_GT(report.freeBytes, 0u) << "capacity recovered";
    EXPECT_EQ(stats().liveVersion.load(), version);
    EXPECT_EQ(stats().errors.load(), 0u);
}

// Review fix 4: A -> B -> A -> C before the fades end, with a new live version
// after the first A: every picture keeps its own live content at its own
// opacity. The first A picture still shows version 1 (the second A got a live
// bin of its own instead of re-binning the one a fading layer draws).
TEST_F(LiveNode, ReturningToATickNeverMutatesALiveBinAFadingPictureDraws) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t open = T - 5 * minute;
    frame().view = {double(T - 30 * minute), double(T), 99'800, 100'200};
    frame().crossfadeMs = 60'000;
    show(tf, {tile1}, open, open, open, 10'000, 1);
    frame().tickUnits = 500; // A
    ASSERT_TRUE(render(3));
    frame().tickUnits = 1000; // B
    ASSERT_TRUE(render());
    frame().live = spans.live(tf, open, open, 25'000, 2); // a new version while A fades
    ASSERT_TRUE(render());
    frame().tickUnits = 500; // A again
    ASSERT_TRUE(render());
    frame().tickUnits = 2000; // C
    ASSERT_TRUE(render());
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // the newest fade has started too
    ASSERT_TRUE(render());
    std::map<uint8_t, Segment> live;
    for (const auto &seg : stats().segments())
        if (seg.kind == Segment::Live) live[seg.layer] = seg;
    ASSERT_EQ(live.size(), 4u) << "the current picture and three fading pictures each draw a live bin";
    EXPECT_EQ(live[1].liveVersion, 1u) << "the first A picture keeps the content it faded out with";
    EXPECT_EQ(live[3].liveVersion, 2u) << "the second A picture";
    EXPECT_NE(live[1].bin, live[3].bin);
    EXPECT_EQ(live[0].opacity, 1.0f);
    EXPECT_LT(live[1].opacity, live[2].opacity) << "older layers fade further";
    EXPECT_LT(live[2].opacity, live[3].opacity);
    EXPECT_LT(live[3].opacity, 1.0f);
    EXPECT_EQ(stats().missingDraws.load(), 0u);
    EXPECT_EQ(stats().unpinnedDraws.load(), 0u);
}

// ---------------------------------------------------------------- transitions
// The live bin is an ordinary drawn entry: on a tick change the old tick's live
// bin fades with its layer while the new tick bins the live window in the same
// frame (new versions keep arriving during the fade); on a timeframe switch the
// held picture keeps the old timeframe's live bin until the new spans are ready.
// Every drawn bin stays resident with its sources. Without the retention rules
// (the node then frees what the target does not use) the same scenario draws
// content that is gone.
struct RetentionRun {
    uint64_t framesDrawnNotResident = 0, fadingLiveFrames = 0, heldLiveFrames = 0, sameFrameLiveTick = 0;
};
RetentionRun runLiveRetention(Spans &spans, Scene &scene) {
    RetentionRun r;
    auto &f = scene.host->frame;
    const auto &st = *scene.host->stats;
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t open = T - 5 * minute;
    f.view = {double(T - 30 * minute), double(T), 99'800, 100'200};
    f.tickUnits = 500;
    f.crossfadeMs = 60'000; // the fade outlasts the scenario
    uint64_t version = 0;
    auto show = [&](int64_t tfMs, std::vector<FakeSpans::Span> list, std::shared_ptr<const LiveSnapshot> live) {
        spans.fake.availableEndMs = open + minute;
        f.tfMs = tfMs;
        f.spans = spans.fake.set(tfMs, std::move(list));
        f.live = std::move(live);
    };
    auto check = [&] {
        if (!subset(st.drawnIds(), st.residentIds())) ++r.framesDrawnNotResident;
    };
    show(tf, {spans.span(tf, tile1, open)}, spans.live(tf, open, open, 10'000, ++version));
    for (int i = 0; i < 3; ++i) { scene.frame(); check(); }
    // Tick change: $5 -> $10. The $10 live bin is drawn in the same frame.
    f.tickUnits = 1000;
    scene.frame();
    check();
    for (const auto &s : st.segments()) {
        if (s.kind != Segment::Live) continue;
        if (s.layer == 0) ++r.sameFrameLiveTick;
        if (s.layer > 0) ++r.fadingLiveFrames;
    }
    for (int i = 0; i < 4; ++i) { // new versions during the fade
        f.live = spans.live(tf, open, open, 12'000 + 2'000 * i, ++version);
        scene.frame();
        check();
        for (const auto &s : st.segments()) r.fadingLiveFrames += s.kind == Segment::Live && s.layer > 0;
    }
    // Timeframe switch to 5m with nothing built yet: the 1m picture (with its live
    // bin) holds; the controller's live window is gone until it composes 5m.
    show(5 * minute, {}, nullptr);
    for (int i = 0; i < 4; ++i) {
        scene.frame();
        check();
        bool live = false;
        for (const auto &s : st.segments()) live = live || (s.kind == Segment::Live && s.layer == 0);
        r.heldLiveFrames += st.holding.load() && live;
    }
    return r;
}
TEST_F(LiveNode, LiveBinIsRetainedThroughHeldAndFadingPictures) {
    {
        const auto r = runLiveRetention(spans, *scene);
        EXPECT_EQ(r.framesDrawnNotResident, 0u);
        EXPECT_EQ(r.sameFrameLiveTick, 1u) << "the new tick's live bin draws in the frame of the tick change";
        EXPECT_GE(r.fadingLiveFrames, 5u) << "the old tick's live bin fades with its layer";
        EXPECT_EQ(r.heldLiveFrames, 4u) << "the held picture keeps the old timeframe's live bin";
        EXPECT_EQ(stats().missingDraws.load(), 0u);
        EXPECT_EQ(stats().unpinnedDraws.load(), 0u) << "drawn live bins keep their buffer sets";
        EXPECT_EQ(stats().errors.load(), 0u);
    }
    HeatmapTileNode::setRetentionDisabledForTest(true);
    {
        Scene other;
        ASSERT_TRUE(other.create(QSize(480, 200))) << other.error.toStdString();
        other.host->frame.view = frame().view;
        const auto r = runLiveRetention(spans, other);
        EXPECT_GT(other.host->stats->missingDraws.load() + r.framesDrawnNotResident, 0u)
            << "without retention, the fading and held live bins are dropped";
    }
    HeatmapTileNode::setRetentionDisabledForTest(false);
}

// ---------------------------------------------------------------- cost
// No QRhiBuffer churn in steady state: after one warm-up cycle (rollover, commit,
// L advance), 60 live versions through another full cycle reuse the live buffer
// sets, the pass uniforms and the live bin's cells: zero creations. Frames with
// no new version do no live work (no upload, no bin pass).
TEST_F(LiveNode, SixtyLiveUpdatesCreateNoBuffersAndIdleFramesDoNoLiveWork) {
    const int64_t tf = minute, T = kEpoch + 2 * kTileMs, tile1 = tiles::tileOfBucket(T / tf) - 1;
    const int64_t m0 = T - 12 * minute;
    frame().view = {double(T - 30 * minute), double(T), 99'800, 100'200};
    uint64_t version = 0;
    // One minute cycle starting at `open`: the minute grows, rolls over (pending),
    // commits, then L advances. `updates` versions in total.
    auto cycle = [&](int64_t open, int updates, int framesPerUpdate) {
        for (int u = 0; u < updates; ++u) {
            const int phase = u * 4 / updates; // 0-1 growing, 2 rolled over, 3 committed + L advanced
            const uint64_t observed = 2'000 + uint64_t(u % (updates / 2)) * 55'000 / uint64_t(updates / 2);
            if (phase < 2) show(tf, {tile1}, open, open, open, observed, ++version);
            else if (phase == 2) show(tf, {tile1}, open, open, open + minute, observed / 4 + 1'000, ++version);
            else show(tf, {tile1}, open + minute, open + minute, open + minute, observed / 4 + 15'000, ++version);
            for (int f = 0; f < framesPerUpdate; ++f) ASSERT_TRUE(render());
        }
    };
    cycle(m0, 20, 2); // warm-up: capacities settle
    const auto &st = stats();
    const uint64_t created = st.liveBufferCreations.load(), uploads = st.liveUploads.load();
    cycle(m0 + minute, 60, 1);
    EXPECT_EQ(st.liveUploads.load() - uploads, 60u);
    EXPECT_EQ(st.liveBufferCreations.load(), created) << "live buffers are reused in place";
    EXPECT_LE(st.liveSets.load(), 4u) << "two buffer sets per source rotate";
    EXPECT_EQ(st.liveVersion.load(), version) << "each version is drawn";
    // Idle frames: the same snapshot, no live work.
    const uint64_t passes = st.liveBinPasses.load(), uploadsNow = st.liveUploads.load();
    ASSERT_TRUE(render(10));
    EXPECT_EQ(st.liveUploads.load(), uploadsNow);
    EXPECT_EQ(st.liveBinPasses.load(), passes);
    EXPECT_EQ(st.errors.load(), 0u);
}

// ---------------------------------------------------------------- disconnect
// Owner decision 3: while disconnected, the last forming column stays drawn,
// frozen. The real controller on FakeChunkTransport: chunks and live frames
// arrive, the node draws the forming minute; the transport goes offline, time
// passes (the controller polls its live cadence) and the same column keeps
// drawing from the same version, with no live work and no loading over it.
// After reconnecting (fresh availability, a new-epoch frame) a new version draws.
class LiveNodeWithController : public testing::Test {
protected:
    ChunkStore store;
    FakeChunkTransport transport;
    ChunkFetcher fetcher{store, transport};
    std::deque<std::function<void()>> jobs;
    std::unique_ptr<SpanSourceCache> cache;
    std::unique_ptr<HeatmapSourceController> controller;
    std::unique_ptr<Scene> scene;
    Recording rec;
    size_t answered = 0;
    int64_t now = 1'000'000, cutoff = 0;
    const int64_t T = kEpoch + 2 * kTileMs;
    void SetUp() override {
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty())
            GTEST_SKIP() << "GPU case skipped: " << why.toStdString();
        cutoff = T - 10 * minute;
        SpanSourceCache::Options options;
        options.executor = [this](std::function<void()> job, int) { jobs.push_back(std::move(job)); };
        cache = std::make_unique<SpanSourceCache>(options);
        HeatmapSourceController::Options c;
        c.capacityPollMs = 0;
        c.nowMs = [this] { return now; };
        c.composeNowNs = [] { return int64_t(0); };
        controller = std::make_unique<HeatmapSourceController>(store, fetcher, *cache, c);
        transport.goOnline();
        transport.push(availability(kEpoch, cutoff));
        drain();
        scene = std::make_unique<Scene>();
        ASSERT_TRUE(scene->create(QSize(480, 200))) << scene->error.toStdString();
        auto &f = scene->host->frame;
        f.capacity = controller->capacity();
        f.tfMs = minute;
        f.tickUnits = 500;
        f.view = {double(T - 40 * minute), double(T), 99'800, 100'200};
        controller->setView(kSymbol, minute, f.view.timeLoMs, f.view.timeHiMs);
    }
    void TearDown() override {
        scene.reset();
        controller.reset();
        jobs.clear();
        cache.reset();
        drain();
    }
    static void drain() {
        for (int i = 0; i < 48; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    ChunkFramePtr chunk(const ChunkKey &key) const {
        auto f = std::make_shared<ChunkFrame>();
        f->key = key;
        const int64_t through = std::clamp(cutoff, key.startMs, key.startMs + kHourMs);
        f->state = {through == key.startMs + kHourMs, through, 1};
        f->contentHash = uint64_t(through);
        f->columns = minuteColumns(key.source, key.startMs, std::max(through, key.startMs + minute), 1, rec.shape);
        f->columns.columns.erase(std::remove_if(f->columns.columns.begin(), f->columns.columns.end(),
                                                [&](const auto &c) { return c.bucketStartMs >= through; }),
                                 f->columns.columns.end());
        f->columns.endMs = key.startMs + kHourMs;
        f->columns.scannedRanges.clear();
        if (through > key.startMs) f->columns.scannedRanges = {{key.startMs, through}};
        return f;
    }
    // The live tail: minutes [cutoff, open], the open one observed `observed` ms.
    ChunkFramePtr tail(const std::string &source, int64_t open, uint64_t revision, uint64_t observed) const {
        auto f = std::make_shared<ChunkFrame>();
        f->kind = ChunkKind::LiveColumn;
        f->key = {kSymbol, source, kMinuteMs, recording::floorDiv(open, kHourMs) * kHourMs};
        f->state = {false, cutoff, revision};
        f->columns = rec.minutes(source, cutoff, open, observed);
        for (auto &c : f->columns.columns) c.flags |= recording::kProvisional;
        return f;
    }
    void sendLive(int64_t open, uint64_t revision, uint64_t observed) {
        ASSERT_FALSE(transport.liveRequests.empty()) << "the controller subscribed the live edge";
        for (const auto *source : {&kCoarse, &kFine})
            transport.replyLive(transport.liveRequests.back().id, tail(*source, open, revision, observed));
        drain();
    }
    bool frame() {
        drain();
        while (answered < transport.requests.size()) {
            const auto request = transport.requests[answered++];
            for (size_t i = 0; i < request.starts.size(); ++i) transport.reply(request.id, chunk(request.key(i)));
        }
        drain();
        for (int round = 0; round < 8 && !jobs.empty(); ++round) {
            auto queued = std::move(jobs);
            jobs.clear();
            for (auto &job : queued) job();
            drain();
        }
        scene->host->frame.spans = controller->latestSnapshot();
        scene->host->frame.live = controller->latestLive();
        if (!scene->frame()) return false;
        controller->pollCapacity();
        drain();
        return true;
    }
    void tick(int64_t ms = 1000) {
        now += ms;
        controller->pollLive();
        drain();
    }
    // The open minute's column is drawn by the live bin (not loading).
    bool formingDrawn(int64_t open) const {
        const int64_t mid = open + minute / 2;
        bool live = false, loading = false;
        for (const auto &s : scene->host->stats->segments()) {
            if (s.layer != 0 || s.loMs > mid || s.hiMs <= mid) continue;
            live = live || s.kind == Segment::Live;
            loading = loading || s.kind == Segment::Loading;
        }
        return live && !loading;
    }
};
TEST_F(LiveNodeWithController, DisconnectedKeepsTheLastFormingColumnDrawnFrozen) {
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(frame());
    const int64_t open = cutoff + 2 * minute; // two pending minutes and the open one
    sendLive(open, 10, 20'000);
    for (int i = 0; i < 6; ++i) {
        tick();
        ASSERT_TRUE(frame());
    }
    sendLive(open, 11, 30'000);
    for (int i = 0; i < 4; ++i) {
        tick();
        ASSERT_TRUE(frame());
    }
    const auto &st = *scene->host->stats;
    ASSERT_TRUE(controller->latestLive()) << "the controller published the live window";
    ASSERT_TRUE(formingDrawn(open)) << "the forming minute is drawn by the live bin";
    const uint64_t version = st.liveVersion.load(), uploads = st.liveUploads.load(), passes = st.liveBinPasses.load();
    ASSERT_GT(version, 0u);
    const QImage before = scene->image;
    // Disconnect: the live edge freezes, time moves past the minute.
    transport.goOffline();
    drain();
    for (int i = 0; i < 90; ++i) {
        tick();
        ASSERT_TRUE(frame());
        ASSERT_TRUE(formingDrawn(open)) << "frame " << i << ": the last forming column stays drawn";
    }
    EXPECT_EQ(st.liveVersion.load(), version) << "frozen: no new version while disconnected";
    EXPECT_EQ(st.liveUploads.load(), uploads) << "no live work while frozen";
    EXPECT_EQ(st.liveBinPasses.load(), passes);
    // The forming column's pixels are unchanged.
    const int x = int((double(open + minute / 2) - scene->host->frame.view.timeLoMs) /
                      (scene->host->frame.view.timeHiMs - scene->host->frame.view.timeLoMs) * 480);
    int same = 0, compared = 0;
    for (int y = 10; y < 190; y += 4) {
        ++compared;
        same += scene->image.pixelColor(x, y) == before.pixelColor(x, y);
    }
    EXPECT_EQ(same, compared) << "the frozen column draws the same cells";
    // Reconnect: fresh availability and a new-epoch frame draw a new version.
    transport.goOnline();
    transport.push(availability(kEpoch, cutoff));
    drain();
    for (int i = 0; i < 4; ++i) {
        tick();
        ASSERT_TRUE(frame());
    }
    sendLive(open + minute, 1, 5'000); // a restarted server may lower the revision
    for (int i = 0; i < 4; ++i) {
        tick();
        ASSERT_TRUE(frame());
    }
    EXPECT_GT(st.liveVersion.load(), version) << "a new version after reconnecting";
    EXPECT_TRUE(formingDrawn(open + minute));
    EXPECT_EQ(st.errors.load(), 0u);
}
} // namespace

int main(int argc, char **argv) {
    // Offscreen unless set: Vulkan needs a real platform plugin (QT_QPA_PLATFORM=windows|xcb).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "[sentinel] " << lab::describeRhi().toStdString() << std::endl;
    return RUN_ALL_TESTS();
}
