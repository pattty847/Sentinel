#pragma once
#include "../../core/heatmap/ChartSurface.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"
#include <map>
#include <optional>

// GUI-thread series storage. The merge decision itself has no mutable state,
// clock, Qt signals or rendering dependency.
//
// Retention, per series: a pan never trims. Bars stay until a cap needs room: kMaxBuckets
// entries per map, or retainBytes of bars outside the kept span. The kept span is what the
// pager keeps loaded for the view (FlowBackfillState's window: the view, its paging margin
// and the CVD anchor), so a trim never makes the pager request the same buckets again.
// A trim takes the bucket farthest from the view first, measured in that side's padding:
// two screens on each side plus two more toward the last pan, rounded out to whole buckets.
// Bars beyond the padding go first. Without a view only kMaxBuckets applies, oldest first.
// Every trim is reported once through takeTrimmed() so the pager forgets that coverage.
class FlowSeriesBuffer {
public:
    using Record = SentinelStreamClient::FlowRecord;
    using Batch = SentinelStreamClient::FlowBatch;
    using Range = SentinelStreamClient::FlowRange;
    enum class Merge { Drop, Replace };
    static Merge merge(const Record* held, const Record& incoming, int64_t sealedThroughMs);
    static constexpr int64_t kRefetchIntervalMs = 30'000;
    // Bytes of bars kept outside the kept span, per chart (approxBarBytes).
    static constexpr size_t kRetainBytes = size_t(32) << 20;
    static constexpr int64_t kPadScreens = 2, kPanPadScreens = 2;
    explicit FlowSeriesBuffer(size_t retainBytes = kRetainBytes) : m_retainBytes(retainBytes) {}
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
        // Buckets marked Partial because the server dropped a live frame for this client,
        // with the coverage entry the mark replaced (none when the bucket had none).
        std::map<int64_t, std::optional<Range>> dropped;
    };
    // Returns buckets requiring an authoritative re-fetch after live coverage loss.
    std::vector<int64_t> apply(const Batch& batch, int64_t nowMs);
    void setCoverage(const QString& symbol, int64_t tf, const Range& range);
    // The server dropped a live frame for bucket: Partial until a later live frame for it
    // (frames carry the whole bucket) restores the coverage the mark replaced, or a history
    // page replaces it. Partial from any other source stays until authoritative evidence
    // resolves it, including across a drop and its recovery.
    void markDropped(const QString& symbol, int64_t tf, int64_t bucket);
    const Series* series(int64_t tf) const;
    const QString& symbol() const { return m_symbol; }
    int64_t sealedThroughMs() const { return m_sealedThroughMs; }
    void clear();
    // The visible range [viewFromMs, viewToMs) and the span the pager keeps loaded for it
    // [keepFromMs, keepToMs), never trimmed. Trims at once if a cap is exceeded.
    void setView(int64_t tf, int64_t viewFromMs, int64_t viewToMs, int64_t keepFromMs, int64_t keepToMs);
    // The padded window around the last view: trims start beyond it, farther side first.
    struct Padding { int64_t fromMs = 0, toMs = 0; int panDirection = 0; };
    std::optional<Padding> padding(int64_t tf) const;
    // Every bucket before belowMs and from fromMs on was trimmed since the last call.
    struct Trim { int64_t belowMs = INT64_MIN, fromMs = INT64_MAX; };
    std::optional<Trim> takeTrimmed(int64_t tf);
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
    struct View {
        int64_t viewFrom = 0, viewTo = 0, keepFrom = 0, keepTo = 0, padFrom = 0, padTo = 0;
        int panDirection = 0;
    };
    void enforceRetention(int64_t tf, Series&);
    size_t m_retainBytes;
    QString m_symbol;
    int64_t m_sealedThroughMs = 0;
    std::map<int64_t, Series> m_series;
    std::map<int64_t, View> m_views;
    std::map<int64_t, Trim> m_trims;
};
