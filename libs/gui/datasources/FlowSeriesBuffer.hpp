#pragma once
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
    static constexpr size_t kMaxBuckets = 8192;
    struct Series {
        std::map<int64_t, Record> bars;
        std::map<int64_t, Range> coverage;
        std::map<int64_t, int64_t> lossRefetchAt;
        std::map<int64_t, uint32_t> losses;
    };
    // Returns buckets requiring an authoritative re-fetch after live coverage loss.
    std::vector<int64_t> apply(const Batch& batch, int64_t nowMs);
    const Series* series(int64_t tf) const;
    int64_t sealedThroughMs() const { return m_sealedThroughMs; }
    void clear();
    void retainWindow(int64_t tf, int64_t fromMs, int64_t toMs);
private:
    QString m_symbol;
    int64_t m_sealedThroughMs = 0;
    std::map<int64_t, Series> m_series;
    std::map<int64_t, std::pair<int64_t, int64_t>> m_windows;
};
