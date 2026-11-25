#include "FlowChunkPlan.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <algorithm>
#include <tuple>
namespace sentinel::flow {
namespace w = protocol::chunks;
bool validWant(const FlowChunkWant& v) {
    return w::validInstrument(v.instrument) && (v.channel == "flow.rows" || v.channel == "flow.summary") &&
        v.timeframeMs >= heatmap::kMinuteMs && v.timeframeMs <= heatmap::kDayMs &&
        v.timeframeMs % heatmap::kMinuteMs == 0 && heatmap::kDayMs % v.timeframeMs == 0 &&
        v.fromMs >= recording::kHmc2MinMs && v.toMs <= recording::kHmc2EndMs && v.fromMs < v.toMs;
}
size_t flowChunkEstimate(const w::Key& key) {
    // Decoded summary: 75 bytes/bar plus grids/coverage. Rows use section 6's
    // 63 KB minute hour, and section 12's measured coarse payloads expanded to
    // fixed-width fields (roughly 3x). Warm keys use their allocation instead.
    if (key.channel == "flow.summary") return size_t(heatmap::chunkSpanMs(key.levelMs) / key.levelMs) * 75 + 1024;
    if (key.levelMs == heatmap::kMinuteMs) return 64ull << 10;
    if (key.levelMs == heatmap::kHourMs) return 320ull << 10;
    return 1280ull << 10;
}
std::vector<PlannedFlowChunk> planFlowChunks(const FlowChunkWant& v, const w::Availability& a,
    const std::function<size_t(const w::Key&)>& estimate, size_t budget) {
    std::vector<PlannedFlowChunk> out;
    if (!validWant(v) || a.instrument != v.instrument || !a.channels.contains(v.channel)) return out;
    const auto width = v.toMs - v.fromMs;
    const auto lo = std::max(recording::kHmc2MinMs, v.fromMs - width * (v.pan < 0 ? 4 : 2));
    const auto hi = std::min(recording::kHmc2EndMs, v.toMs + width * (v.pan > 0 ? 4 : 2));
    std::vector<w::Run> remaining{{lo, hi}};
    auto levels = a.channels.at(v.channel).levels;
    std::sort(levels.begin(), levels.end(), [](const auto& x, const auto& y) { return x.levelMs > y.levelMs; });
    for (const auto& level : levels) {
        if (v.timeframeMs % level.levelMs) continue;
        std::vector<w::Run> next;
        for (const auto interval : remaining) {
            auto cursor = interval.fromMs;
            for (const auto& run : level.sealed) {
                const auto from = std::max(interval.fromMs, run.fromMs), to = std::min(interval.toMs, run.toMs);
                if (from >= to) continue;
                const auto first = heatmap::chunkStartOf(level.levelMs, from);
                const auto end = heatmap::chunkStartOf(level.levelMs, to - 1) + level.chunkSpanMs;
                if (cursor < first) next.push_back({cursor, first});
                for (auto start = first; start < end; start += level.chunkSpanMs) {
                    const bool visible = start < v.toMs && start + level.chunkSpanMs > v.fromMs;
                    const bool right = start >= v.toMs;
                    const auto distance = right ? uint64_t(start - v.toMs) : uint64_t(std::max<int64_t>(0, v.fromMs - start - level.chunkSpanMs));
                    out.push_back({{v.instrument, v.channel, level.levelMs, v.channel == "flow.rows" ? 1u : 0u, start},
                        visible ? 0 : ((right == (v.pan > 0)) ? 1 : 2), visible ? uint64_t(recording::kHmc2EndMs - start) : distance});
                }
                cursor = std::max(cursor, end);
            }
            if (cursor < interval.toMs) next.push_back({cursor, interval.toMs});
        }
        remaining = std::move(next);
    }
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) {
        return std::tie(x.tier, x.order, x.key) < std::tie(y.tier, y.order, y.key);
    });
    size_t bytes = 0;
    bool paddingStopped = false;
    std::erase_if(out, [&](const auto& item) {
        if (!item.tier) return false;
        if (paddingStopped) return true;
        const auto n = std::max<size_t>(1, estimate(item.key));
        if (n > budget - bytes) { paddingStopped = true; return true; }
        bytes += n; return false;
    });
    return out;
}
}
