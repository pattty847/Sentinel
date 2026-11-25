#pragma once
#include "../../core/heatmap/ChartSurface.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"
#include "flow/FlowChunkView.hpp"
#include <map>

// GUI-owned, bounded composed viewport, replaced atomically after worker composition.
// Immutable historical bodies live only in FlowChunkClient's cache. apply() is the
// bounded live/synthetic source adapter used by non-remote feeds; it never fetches.
class FlowSeriesBuffer {
public:
    using Record = SentinelStreamClient::FlowRecord;
    using Batch = SentinelStreamClient::FlowBatch;
    using Range = SentinelStreamClient::FlowRange;
    static constexpr size_t kMaxBuckets = heatmap::kMaxChartSurfaceBars;
    struct Series {
        int64_t anchorMs = -1, prefixThroughMs = 0, loadedFromMs = 0;
        double prefixDelta = 0;
        std::map<int64_t, Record> bars;
        std::map<int64_t, Range> coverage;
    };
    void apply(const Batch&, int64_t nowMs);
    void setCoverage(const QString&, int64_t tf, const Range&);
    const Series* series(int64_t tf) const;
    const QString& symbol() const { return m_symbol; }
    void clear();
    void replace(const QString&, int64_t, sentinel::flow::FlowViewSnapshot);
    // Approximate heap held, from sizes and capacities (client.mem probe). Map nodes
    // count their value plus kMapNodeOverheadBytes; string heap is not counted.
    static constexpr size_t kMapNodeOverheadBytes = 32;
    struct MemoryApprox {
        size_t series = 0, bars = 0, rows = 0, coverageRanges = 0;
        size_t rowBytes = 0, notableBytes = 0, nodeBytes = 0;
        int64_t coveredFromMs = 0, coveredToMs = 0; // span of all coverage ranges, 0 when none
        size_t totalBytes() const { return rowBytes + notableBytes + nodeBytes; }
    };
    MemoryApprox approxMemory() const;
    // One bar's share of approxMemory().totalBytes(): its map node, rows and notables.
    static size_t approxBarBytes(const Record&);
private:
    QString m_symbol;
    std::map<int64_t, Series> m_series;
};
