#include "HeatmapLabelMatch.hpp"
#include "heatmap/DrawPieces.hpp"
#include <algorithm>
#include <climits>

namespace heatmap::gpu {
namespace {
enum : uint8_t { kNone = 0, kSpan = 1, kLive = 2 };
// Per-thread scratch (render thread and GUI thread each have one): no allocation
// once warm.
struct Scratch {
    std::vector<DrawSpan> spans;
    std::vector<DrawLive> live;
    std::vector<uint8_t> targetKind;
    Scratch() {
        spans.reserve(256);
        live.reserve(4);
        targetKind.reserve(16'384);
    }
};
} // namespace

size_t matchLabelColumns(const LabelCells &labels, const std::vector<HeatmapTileStats::Segment> &drawn,
                         const SpanSet *target, const LiveSnapshot *targetLive, std::vector<uint8_t> &out) {
    const auto &k = labels.key;
    out.assign(k.columns, 0);
    const int64_t tf = k.tfMs;
    if (tf <= 0 || labels.liveColumns.size() != k.columns) return 0;
    thread_local Scratch scratch;
    // This frame's target picture, column by column: which bin kind draws each
    // bucket (the same draw clip as the node and the label builder). The node may
    // switch to it in this very frame (prepare() runs after the label layout).
    auto &kind = scratch.targetKind;
    kind.assign(k.columns, kNone);
    scratch.spans.clear();
    scratch.live.clear();
    if (target && target->tfMs == tf) {
        for (size_t i = 0; i < target->spans.size(); ++i) {
            const auto &span = target->spans[i];
            if (span.id.tfMs != tf) continue;
            int64_t completeEnd = INT64_MAX;
            for (const auto &s : span.sources)
                if (s.build) completeEnd = std::min(completeEnd, s.build->completeEndMs);
            if (completeEnd != INT64_MAX) scratch.spans.push_back({i, span.id.tile, tf, completeEnd});
        }
    }
    const bool liveTarget = targetLive && targetLive->tfMs == tf && (!target || targetLive->symbol == target->symbol) &&
                            !targetLive->sources.empty();
    const uint64_t targetVersion = liveTarget ? targetLive->version : 0;
    if (liveTarget) {
        int64_t start = INT64_MAX, end = 0;
        for (const auto &s : targetLive->sources)
            if (s.columns) {
                start = std::min(start, s.startMs);
                end = std::max(end, recording::floorDiv(s.openEndMs + tf - 1, tf) * tf);
            }
        if (start < end) scratch.live.push_back({0, tf, start, end});
    }
    drawPieces(scratch.spans, scratch.live, tf, [&](DrawPiece p) {
        const int64_t b0 = std::max(k.firstBucket, recording::floorDiv(p.loMs + tf - 1, tf));
        const int64_t b1 = std::min(k.firstBucket + int64_t(k.columns), recording::floorDiv(p.hiMs + tf - 1, tf));
        for (int64_t b = b0; b < b1; ++b) kind[size_t(b - k.firstBucket)] = p.live ? kLive : kSpan;
    });
    // The target span content of a tile (the node draws the target's complete bin).
    int64_t cachedTile = INT64_MIN;
    uint64_t cachedTarget = 0, cachedLabels = 0;
    auto contents = [&](int64_t tile) {
        if (tile == cachedTile) return;
        cachedTile = tile;
        cachedTarget = cachedLabels = 0;
        if (target && target->tfMs == tf)
            for (const auto &span : target->spans)
                if (span.id.tfMs == tf && span.id.tile == tile) { cachedTarget = spanContentId(span); break; }
        for (const auto &[t, id] : labels.spanContent)
            if (t == tile) { cachedLabels = id; break; }
    };
    // A column matches when the node's last frame, the labels and the target all
    // take it from the same kind of bin with the same content.
    size_t matched = 0;
    for (const auto &s : drawn) {
        if (s.layer != 0 || s.kind == HeatmapTileStats::Segment::Loading || !(s.hiMs > s.loMs)) continue;
        const bool live = s.kind == HeatmapTileStats::Segment::Live;
        // Buckets whose start lies in [lo, hi), as drawPieces assigns them.
        const int64_t b0 = std::max(k.firstBucket, recording::floorDiv(s.loMs + tf - 1, tf));
        const int64_t b1 = std::min(k.firstBucket + int64_t(k.columns), recording::floorDiv(s.hiMs + tf - 1, tf));
        for (int64_t b = b0; b < b1; ++b) {
            const size_t x = size_t(b - k.firstBucket);
            if (labels.liveColumns[x] != live || kind[x] != (live ? kLive : kSpan)) continue;
            bool ok = false;
            if (live) {
                ok = s.liveVersion != 0 && s.liveVersion == k.liveVersion && s.liveVersion == targetVersion;
            } else {
                contents(tiles::tileOfBucket(b));
                ok = s.content != 0 && s.content == cachedLabels && s.content == cachedTarget;
            }
            if (ok && !out[x]) {
                out[x] = 1;
                ++matched;
            }
        }
    }
    return matched;
}
} // namespace heatmap::gpu
