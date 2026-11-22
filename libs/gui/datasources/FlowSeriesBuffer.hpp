#pragma once
#include "../../core/heatmap/ChartSurface.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"
#include <map>

// GUI-thread series storage. The merge decision itself has no mutable state,
// clock, Qt signals or rendering dependency.
class FlowSeriesBuffer {
public:
    using Record = SentinelStreamClient::FlowRecord;
    using Batch = SentinelStreamClient::FlowBatch;
    using Range = SentinelStreamClient::FlowRange;
    enum class Merge { Drop, Replace };
    static Merge merge(const Record* held, const Record& incoming, int64_t sealedThroughMs);
    static constexpr int64_t kRefetchIntervalMs = 30'000;
    // Every bar of the widest allowed view (retention and backfill share it).
    static constexpr size_t kMaxBuckets = heatmap::kMaxChartSurfaceBars;
    // Buckets of tf that [start, end) touches: floor(start / tf) .. floor((end - 1) / tf),
    // the endpoints FlowBackfillState::setWindow uses.
    static int64_t bucketsTouching(int64_t start, int64_t end, int64_t tf) {
        if (tf <= 0 || end <= start) return 0;
        const auto floorDiv = [tf](int64_t v) { return v / tf - (v % tf < 0 ? 1 : 0); };
        return floorDiv(end - 1) - floorDiv(start) + 1;
    }
    struct Series {
        std::map<int64_t, Record> bars;
        std::map<int64_t, Range> coverage;
        std::map<int64_t, int64_t> lossRefetchAt;
        std::map<int64_t, uint32_t> losses;
    };
    // Returns buckets requiring an authoritative re-fetch after live coverage loss.
    std::vector<int64_t> apply(const Batch& batch, int64_t nowMs);
    void setCoverage(const QString& symbol, int64_t tf, const Range& range);
    const Series* series(int64_t tf) const;
    const QString& symbol() const { return m_symbol; }
    int64_t sealedThroughMs() const { return m_sealedThroughMs; }
    void clear();
    void retainWindow(int64_t tf, int64_t fromMs, int64_t toMs);
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
private:
    QString m_symbol;
    int64_t m_sealedThroughMs = 0;
    std::map<int64_t, Series> m_series;
    std::map<int64_t, std::pair<int64_t, int64_t>> m_windows;
};
