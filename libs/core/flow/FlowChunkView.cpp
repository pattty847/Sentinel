#include "FlowChunkView.hpp"
#include "heatmap/ChartSurface.hpp"
#include "FlowCompose.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
namespace sentinel::flow {
namespace {
using Status = chunks::Status;
struct Parts {
    int64_t covered = 0;
    bool partial = false, recorded = false, unrecorded = false;
    std::vector<FlowBar> bars;
};
Status coverage(const chunks::FlowChunk& chunk, int slot) {
    for (const auto& run : chunk.coverage)
        if (slot >= run.fromSlot && slot < run.toSlot) return run.status;
    return Status::Building;
}
void collect(std::map<int64_t, Parts>& parts, const chunks::FlowChunk& chunk,
             int64_t tf, int64_t from, int64_t to, bool rows) {
    const auto step = chunk.key.levelMs;
    for (int slot = 0; slot < chunk.slotCount; ++slot) {
        const auto time = chunk.key.startMs + slot * step;
        if (time < from || time >= to) continue;
        auto& p = parts[time / tf * tf];
        p.covered += step;
        const auto status = coverage(chunk, slot);
        p.partial |= status == Status::Partial;
        p.unrecorded |= status == Status::Unrecorded;
        p.recorded |= status != Status::Unrecorded;
    }
    for (size_t i = 0; i < chunk.bars; ++i) {
        const auto time = chunk.key.startMs + chunk.slot[i] * step;
        if (time < from || time >= to) continue;
        const auto& g = chunk.grids[chunk.grid.empty() ? 0 : chunk.grid[i]];
        const auto rowStep = std::max<uint64_t>(1, chunk.key.priceStep);
        FlowBar bar;
        bar.startMs = time; bar.timeframeMs = step;
        bar.priceScale = g.priceScale; bar.baseScale = g.baseScale; bar.quoteScale = g.quoteScale;
        bar.nativeTick = double(g.rowTickUnits) * rowStep / g.priceScale;
        bar.open = double(chunk.open[i]) / g.priceScale; bar.high = double(chunk.high[i]) / g.priceScale;
        bar.low = double(chunk.low[i]) / g.priceScale; bar.close = double(chunk.close[i]) / g.priceScale;
        bar.lowRow = chunk.low[i] / g.rowTickUnits / int64_t(rowStep);
        bar.baseVolume = chunk.baseVol[i]; bar.quoteVolume = chunk.quoteVol[i];
        bar.buyVol = chunk.buyVol[i]; bar.sellVol = chunk.sellVol[i]; bar.delta = bar.buyVol - bar.sellVol;
        bar.count = chunk.count[i];
        if (rows && !chunk.rowBegin.empty()) {
            bar.rows.reserve(chunk.rowBegin[i + 1] - chunk.rowBegin[i]);
            for (auto r = chunk.rowBegin[i]; r < chunk.rowBegin[i + 1]; ++r) {
                const auto& row = chunk.row[r];
                bar.rows.push_back({chunk.offset[r], row.bidVol, row.askVol, row.bidCount, row.askCount});
                bar.maxRowVol = std::max(bar.maxRowVol, row.bidVol + row.askVol);
            }
        }
        parts[time / tf * tf].bars.push_back(std::move(bar));
    }
}
FlowViewBucket sealedBucket(Parts& parts, int64_t tf) {
    FlowViewBucket out;
    out.status = parts.partial || (parts.recorded && parts.unrecorded) ? Status::Partial :
        parts.unrecorded ? Status::Unrecorded : Status::Complete;
    if (!parts.bars.empty()) {
        std::sort(parts.bars.begin(), parts.bars.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
        // The planner can mix stored levels within a target bucket. Their
        // disjoint aggregates compose identically on the finest constituent grid.
        int64_t finest = tf;
        for (const auto& bar : parts.bars) finest = std::min(finest, bar.timeframeMs);
        for (auto& bar : parts.bars) bar.timeframeMs = finest;
        out.value = FlowViewBar{composeFlowBars(parts.bars, tf), 0, out.status == Status::Complete, true};
    }
    return out;
}
}
void FlowChunkView::resetLive() { live_.clear(); }
void FlowChunkView::retention(int64_t from) {
    liveFromMs_ = std::max(liveFromMs_, from);
    std::erase_if(live_, [&](const auto& p) {
        return p.second.value && p.first + p.second.value->bar.timeframeMs <= liveFromMs_;
    });
    std::erase_if(retired_, [&](const auto& p) { return p.second <= liveFromMs_; });
}
void FlowChunkView::live(FlowViewBar incoming, Status status) {
    const auto start = incoming.bar.startMs, end = start + incoming.bar.timeframeMs;
    if (end <= liveFromMs_ || retired_.contains(start)) return;
    const auto held = live_.find(start);
    if (held != live_.end() && held->second.value) {
        const auto& old = *held->second.value;
        if (incoming.revision < old.revision ||
            (incoming.revision == old.revision && old.complete && !incoming.complete)) return;
    }
    live_[start] = {status, std::move(incoming)};
    // One-second flow is RAM only; a quiet source still has a hard bucket cap.
    while (live_.size() > 1802) live_.erase(live_.begin());
}
FlowViewSnapshot FlowChunkView::compose(FlowChunkCache& cache, const FlowChunkWant& want,
                                      const protocol::chunks::Availability& availability, int64_t anchor) {
    FlowViewSnapshot out;
    const auto tf = want.timeframeMs;
    if (tf < 1000 || tf > heatmap::kDayMs || want.fromMs < 0 ||
        want.toMs > INT64_MAX - tf || want.toMs <= want.fromMs ||
        (anchor >= 0 && want.fromMs - anchor > 32 * heatmap::kDayMs)) return out;
    const auto to = (want.toMs - 1) / tf * tf + tf;
    const auto from = std::max(want.fromMs / tf * tf, to - int64_t(heatmap::kMaxChartSurfaceBars) * tf);
    auto scanFrom = anchor >= 0 ? std::min(from, (anchor + tf - 1) / tf * tf) : from;
    // One-second history does not exist. Its cumulative baseline is explicitly
    // the retained RAM window; do not scan a month of missing seconds.
    if (tf == 1000 && !live_.empty()) scanFrom = std::max(scanFrom, std::min(from, live_.begin()->first));
    auto scan = want; scan.fromMs = scanFrom; scan.toMs = to;
    std::map<int64_t, Parts> parts;
    for (const auto& item : planFlowChunks(scan, availability, flowChunkEstimate, 0)) {
        if (auto body = cache.read(item.key)) collect(parts, *body, tf, scanFrom, to, want.channel == "flow.rows");
    }
    // Availability lists sealed chunks, not the journal's recorded extent.
    // In newest-first recovery an older missing hour is still building. Only
    // explicit chunk coverage proves unrecorded; oldest sealed is not that proof.
    out.anchorMs = anchor; out.prefixThroughMs = from; out.loadedFromMs = scanFrom;
    out.prefixComplete = true;
    for (auto time = scanFrom; time < to; time += tf) {
        FlowViewBucket bucket;
        const auto p = parts.find(time);
        if (p != parts.end() && p->second.covered == tf) {
            try { bucket = sealedBucket(p->second, tf); }
            catch (const std::exception& e) {
                static sentinel::log_throttle::Site warning;
                uint32_t suppressed = 0;
                if (warning.admit(5000, sentinel::log_throttle::nowMs(), suppressed))
                    sLog_Warning("Flow composition rejected instrument=" << QString::fromStdString(want.instrument)
                                 << " bucket=" << time << " reason=" << e.what());
            }
            live_.erase(time);
            if (time + tf > liveFromMs_) retired_[time] = time + tf;
        } else if (const auto live = live_.find(time); live != live_.end() && !retired_.contains(time)) {
            bucket = live->second;
        }
        if (time < from) {
            out.prefixComplete &= bucket.status == Status::Complete || bucket.status == Status::Unrecorded;
            if (bucket.status == Status::Building) out.loadedFromMs = time + tf;
            if (bucket.value && bucket.value->bar.baseScale > 0)
                out.prefixDelta += double(bucket.value->bar.delta) / bucket.value->bar.baseScale;
        } else out.buckets.emplace(time, std::move(bucket));
    }
    return out;
}
}
