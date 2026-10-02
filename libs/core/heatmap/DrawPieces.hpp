#pragma once
#include "HeatmapTiles.hpp"
#include <array>
#include <span>

namespace heatmap {
// One picture's already-selected bins (including held/fallback bins). Tokens
// are opaque to this pure clip: the node uses bin ids, the query uses indices.
struct DrawSpan {
    uint64_t token = 0;
    int64_t tile = 0, tfMs = 0, completeEndMs = 0;
};
struct DrawLive {
    uint64_t token = 0;
    int64_t tfMs = 0, startMs = 0, endMs = 0;
};
struct DrawPiece {
    uint64_t token = 0;
    int64_t loMs = 0, hiMs = 0;
    bool live = false;
    bool operator==(const DrawPiece &) const = default;
};
// tf == 0 includes every timeframe of a held picture. The callback overload
// writes into the node's reused vector without allocating temporary pieces.
template<class Emit>
void drawPieces(std::span<const DrawSpan> spans, std::span<const DrawLive> live, int64_t tf, Emit append) {
    std::array<DrawLive, 4> clips{};
    size_t count = 0;
    auto spanEnd = [](const DrawSpan& s) {
        return std::clamp(s.completeEndMs, tiles::tileStartMs(s.tile, s.tfMs), tiles::tileEndMs(s.tile, s.tfMs));
    };
    for (const auto& l : live) {
        if ((tf && l.tfMs != tf) || l.tfMs <= 0 || count == clips.size()) continue;
        int64_t from = l.endMs;
        const auto first = tiles::tileOfBucket(recording::floorDiv(l.startMs, l.tfMs));
        for (int64_t tile = first; tiles::tileStartMs(tile, l.tfMs) < l.endMs && tile < first + 1024; ++tile) {
            const auto ts = tiles::tileStartMs(tile, l.tfMs), te = tiles::tileEndMs(tile, l.tfMs);
            const auto s = std::find_if(spans.begin(), spans.end(), [&](const auto& s) {
                return s.tfMs == l.tfMs && s.tile == tile;
            });
            const auto e = s == spans.end() ? ts : spanEnd(*s);
            if (e < te) { from = std::max(l.startMs, e); break; }
        }
        if (from >= l.endMs) continue;
        auto stop = l.endMs;
        for (const auto& s : spans) {
            const auto ts = tiles::tileStartMs(s.tile, s.tfMs);
            if (s.tfMs == l.tfMs && ts >= from && spanEnd(s) > l.endMs) stop = std::min(stop, ts);
        }
        clips[count++] = {l.token, l.tfMs, from, stop};
    }
    for (size_t i = 0; i < count; ++i)
        if (clips[i].endMs > clips[i].startMs) append(DrawPiece{clips[i].token, clips[i].startMs, clips[i].endMs, true});
    for (const auto& s : spans) {
        if (tf && s.tfMs != tf) continue;
        const auto ts = tiles::tileStartMs(s.tile, s.tfMs);
        auto hi = spanEnd(s);
        for (size_t i = 0; i < count; ++i)
            if (clips[i].tfMs == s.tfMs && ts < clips[i].endMs) hi = std::min(hi, std::max(ts, clips[i].startMs));
        append(DrawPiece{s.token, ts, hi, false});
    }
}
inline std::vector<DrawPiece> drawPieces(std::span<const DrawSpan> spans, std::span<const DrawLive> live, int64_t tf) {
    std::vector<DrawPiece> out;
    out.reserve(spans.size() + live.size());
    drawPieces(spans, live, tf, [&](DrawPiece p) { out.push_back(p); });
    return out;
}
} // namespace heatmap
