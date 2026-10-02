#include "HeatmapLabelMatch.hpp"
#include <algorithm>
#include <climits>

namespace heatmap::gpu {
size_t matchLabelColumns(const LabelCells &labels, const std::vector<HeatmapTileStats::Segment> &drawn,
                         const SpanSet *target, const LiveSnapshot *targetLive, std::vector<uint8_t> &out) {
    const auto &k = labels.key;
    out.assign(k.columns, 0);
    const int64_t tf = k.tfMs;
    if (tf <= 0 || labels.liveColumns.size() != k.columns) return 0;
    // Live: the version drawn and the target's; a different target may move the
    // live window [L, end) this frame, so nothing from its earliest start on matches.
    uint64_t drawnLive = 0;
    int64_t drawnLiveLo = INT64_MAX;
    for (const auto &s : drawn)
        if (s.layer == 0 && s.kind == HeatmapTileStats::Segment::Live) {
            drawnLive = s.liveVersion;
            drawnLiveLo = std::min(drawnLiveLo, s.loMs);
        }
    const bool liveTarget = targetLive && targetLive->tfMs == tf && !targetLive->sources.empty();
    const uint64_t targetVersion = liveTarget ? targetLive->version : 0;
    int64_t riskFromMs = INT64_MAX;
    if (targetVersion != drawnLive) {
        riskFromMs = drawnLiveLo;
        if (liveTarget)
            for (const auto &src : targetLive->sources) riskFromMs = std::min(riskFromMs, src.startMs);
    }
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
    size_t matched = 0;
    for (const auto &s : drawn) {
        if (s.layer != 0 || s.kind == HeatmapTileStats::Segment::Loading || !(s.hiMs > s.loMs)) continue;
        const bool live = s.kind == HeatmapTileStats::Segment::Live;
        // Buckets whose start lies in [lo, hi), as drawPieces assigns them.
        const int64_t b0 = std::max(k.firstBucket, recording::floorDiv(s.loMs + tf - 1, tf));
        const int64_t b1 = std::min(k.firstBucket + int64_t(k.columns), recording::floorDiv(s.hiMs + tf - 1, tf));
        for (int64_t b = b0; b < b1; ++b) {
            const size_t x = size_t(b - k.firstBucket);
            if (labels.liveColumns[x] != live || b * tf + tf > riskFromMs) continue;
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
