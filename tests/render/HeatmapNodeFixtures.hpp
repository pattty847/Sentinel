#pragma once
// Synthetic two-source heatmap data for the S5c node and lab tests (no GPU, no
// recording): a coarse source ($5 rows over a wide book, one recorder gap every
// 17 minutes) and a fine source ($1 rows in a band around the mid). The ids are
// the chunk sources of ChunkCodec (data only: production code never branches on
// them). Builders for chunks, span sources and hand-made SpanSets (the "fake
// controller"), and chunk frames for FakeChunkTransport replies.
#include "heatmap/ChunkFetcher.hpp"
#include "heatmap/HeatmapTiles.hpp"
#include "render/heatmap/HeatmapSourceController.hpp"
#include <map>
#include <tuple>

namespace nodefx {
using namespace heatmap;
// Aligned to 5m tiles (320 minutes), so 1m and 5m tile edges coincide.
inline constexpr int64_t kEpoch = ((recording::kHmc2MinMs / (320 * kMinuteMs)) + 3) * (320 * kMinuteMs);
inline constexpr int64_t kTileMs = tiles::kTileColumns * kMinuteMs;
inline const std::string kCoarse = "hmc2.deep", kFine = "hmc2.near";

struct Shape {
    int64_t coarseLo = 95'000, coarseHi = 105'000; // $ range of the coarse ($5) rows
    int64_t fineLo = 99'500, fineHi = 100'500;     // $ range of the fine ($1) band
    int64_t mid = 100'000;
    bool coarseGaps = true;                        // a recorder gap every 17 minutes (coarse only)
};

// Minute columns of one source for [start, end), revision-dependent sizes.
inline SparseColumns minuteColumns(const std::string &source, int64_t start, int64_t end, uint64_t revision = 1,
                                   const Shape &shape = {}) {
    const bool coarse = source == kCoarse;
    const int64_t tick = coarse ? 500 : 100; // price units (priceScale 100)
    const int64_t lo = (coarse ? shape.coarseLo : shape.fineLo) * 100 / tick;
    const int64_t hi = (coarse ? shape.coarseHi : shape.fineHi) * 100 / tick; // exclusive
    const int64_t midRow = shape.mid * 100 / tick;
    SparseColumns out{"BTC-USD", std::string(findChunkSource(source)->hmc2Layer), kMinuteMs, start, end, {}, {{start, end}}};
    for (int64_t t = start; t < end; t += kMinuteMs) {
        const int64_t i = (t - kEpoch) / kMinuteMs;
        if (coarse && shape.coarseGaps && ((i % 17) + 17) % 17 == 3) continue; // scanned, no column: veil
        NativeColumn n;
        n.grid = {coarse ? 7u : 3u, tick, 100};
        n.observedMs = uint64_t(kMinuteMs);
        n.baseRow = lo;
        n.coverage[0] = {{lo, hi - 1, uint64_t(kMinuteMs)}};
        n.coverage[1] = n.coverage[0];
        for (int64_t row = lo; row < hi; ++row) {
            if ((row + i) % 5 == 2) continue;
            const bool ask = row >= midRow;
            const double size = 0.001 * double(1 + (row * 31 + i * 17 + int64_t(revision) * 7) % 97) *
                                (1 + double(std::abs(row - midRow) % 13) * 0.2);
            n.entries.push_back({packRowSide(row, lo, ask), recording::encodeSize(size)});
        }
        out.columns.push_back({t, uint64_t(kMinuteMs), 0, {std::move(n)}});
    }
    validate(out);
    return out;
}

// A stored hour chunk (key source, hour start), as the ChunkStore would hold it.
inline std::shared_ptr<const StoredChunk> storedChunk(const std::string &source, int64_t hourStart, uint64_t generation,
                                                      uint64_t revision = 1, const Shape &shape = {}) {
    auto chunk = std::make_shared<StoredChunk>();
    chunk->key = {"BTC-USD", source, kMinuteMs, hourStart};
    chunk->columns = std::make_shared<const SparseColumns>(minuteColumns(source, hourStart, hourStart + kHourMs,
                                                                         revision, shape));
    chunk->sealed = true;
    chunk->generation = generation;
    chunk->contentHash = revision;
    chunk->bytes = sparseBytes(*chunk->columns);
    return chunk;
}

// A chunk frame for FakeChunkTransport replies.
inline ChunkFramePtr chunkFrame(const ChunkKey &key, uint64_t revision = 1, const Shape &shape = {}) {
    auto frame = std::make_shared<ChunkFrame>();
    frame->key = key;
    const int64_t span = chunkSpanMs(key.source, key.levelMs);
    frame->state = {true, key.startMs + span, revision};
    frame->contentHash = revision;
    frame->columns = minuteColumns(key.source, key.startMs, key.startMs + span, revision, shape);
    return frame;
}

// Availability of both sources (minute level only) over [start, end).
inline ChunkAvailability availability(int64_t start, int64_t end) {
    ChunkAvailability a;
    a.symbol = "BTC-USD";
    a.chunkWireVersion = kChunkWireVersion;
    for (const auto *id : {&kCoarse, &kFine}) {
        protocol::chunkwire::SourceInfo s;
        s.id = *id;
        s.latestGrid = protocol::chunkwire::GridInfo{};
        s.latestGrid->priceScale = 100;
        s.levels.push_back({kMinuteMs, kHourMs, end, start, end - kMinuteMs});
        a.sources.push_back(std::move(s));
    }
    return a;
}

// The fake controller: span builds from synthetic chunks (cached by identity)
// and SpanSets assembled as HeatmapSourceController::publish does.
struct FakeSpans {
    Shape shape;
    int64_t availableStartMs = kEpoch, availableEndMs = kEpoch + 12 * kHourMs;
    std::map<std::tuple<std::string, int64_t, uint64_t>, std::shared_ptr<const StoredChunk>> chunks;
    std::map<std::tuple<int64_t, int64_t, std::string, uint64_t>, SpanSourceBuildPtr> builds;
    uint64_t version = 0;

    SpanSourceBuildPtr build(int64_t tfMs, int64_t tile, const std::string &source, uint64_t revision = 1) {
        auto &slot = builds[{tfMs, tile, source, revision}];
        if (slot) return slot;
        const SpanId span{"BTC-USD", tfMs, tile};
        SpanSourceInput input;
        input.key = {span, source, std::clamp(availableStartMs, span.startMs(), span.endMs()),
                     std::clamp(availableEndMs, span.startMs(), span.endMs()), 100, uint64_t(tile * 31 + tfMs), {}};
        for (int64_t h = recording::floorDiv(span.startMs(), kHourMs) * kHourMs; h < span.endMs(); h += kHourMs) {
            if (h + kHourMs <= availableStartMs || h >= availableEndMs) continue;
            auto &chunk = chunks[{source, h, revision}];
            if (!chunk) chunk = storedChunk(source, h, revision * 1'000'000 + uint64_t(h / kHourMs), revision, shape);
            input.key.generations.push_back({"BTC-USD", source, kMinuteMs, h, chunk->generation, true});
            input.chunks.push_back(chunk);
        }
        std::sort(input.key.generations.begin(), input.key.generations.end());
        slot = buildSpanSource(input);
        return slot;
    }
    // The same build with its CPU image released (after the node reported the upload).
    static SpanSourceBuildPtr released(const SpanSourceBuildPtr &build) {
        auto light = std::make_shared<SpanSourceBuild>(*build);
        light->gpu.reset();
        return light;
    }
    struct Span {
        int64_t tile = 0;
        SpanRank rank;
        std::vector<SpanSourceBuildPtr> sources; // any order; sorted coarsest first
        int64_t tfMs = kMinuteMs;
        std::vector<std::string> failed; // sources whose build failed terminally (no build)
    };
    std::shared_ptr<const SpanSet> set(int64_t tfMs, std::vector<Span> spans, std::vector<SpanId> refused = {}) {
        auto out = std::make_shared<SpanSet>();
        out->version = ++version;
        out->symbol = "BTC-USD";
        out->tfMs = tfMs;
        out->priceScale = 100;
        out->resolution.tfMs = tfMs;
        out->resolution.priceScale = 100;
        out->refused = std::move(refused);
        out->availableStartMs = availableStartMs;
        out->availableEndMs = availableEndMs;
        for (auto &s : spans) {
            SpanSnapshot span{{"BTC-USD", s.tfMs, s.tile}, s.rank, {}, true};
            for (const auto &b : s.sources) {
                span.sources.push_back({b->key.source, b, false, false});
                if (s.tfMs == tfMs) mergeResolution(out->resolution, b->resolution);
            }
            for (const auto &name : s.failed) span.sources.push_back({name, nullptr, false, true});
            span.complete = s.failed.empty();
            std::stable_sort(span.sources.begin(), span.sources.end(), [](const auto &a, const auto &b) {
                const int64_t x = a.build ? a.build->commonUnits : 0, y = b.build ? b.build->commonUnits : 0;
                return x > y;
            });
            out->spans.push_back(std::move(span));
        }
        std::stable_sort(out->spans.begin(), out->spans.end(), [](const auto &a, const auto &b) { return a.rank < b.rank; });
        return out;
    }
    // Both sources of `tiles` at tf, visible (distance from the first) unless ranked otherwise.
    Span both(int64_t tfMs, int64_t tile, SpanRank rank, uint64_t revision = 1) {
        return {tile, rank, {build(tfMs, tile, kCoarse, revision), build(tfMs, tile, kFine, revision)}, tfMs};
    }
};
inline int64_t firstTile(int64_t tfMs = kMinuteMs) { return tiles::tileOfBucket(kEpoch / tfMs); }
} // namespace nodefx
