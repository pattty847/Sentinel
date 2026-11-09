#pragma once
#include "FlowBar.hpp"
#include <deque>
#include <map>
#include <optional>
#include <unordered_set>

namespace sentinel::flow {
// Accepts a bounded trade-id window; bars are materialized in exchange-time order.
class FlowBarBuilder {
public:
    enum class AddResult { Accepted, Duplicate, Rejected };
    // priceScale is recorder integer price units per quote; baseScale and quoteScale
    // are integer volume units per base and quote currency. Quote volume is
    // rounded once per trade, to nearest integer with exact halves upward.
    // Rejected prints are counted and never abort a journal build.
    FlowBarBuilder(int64_t timeframeMs, double nativeTick,
                   int64_t priceScale, int64_t baseScale, int64_t quoteScale);
    AddResult add(const Trade& trade);
    std::optional<FlowBar> bar(int64_t bucketStartMs);
    // Materialize one bucket without copying or modifying the live builder.
    std::optional<FlowBar> snapshotBar(int64_t bucketStartMs) const;
    std::vector<FlowBar> bars();
    // The caller owns the exchange-time clock. After publishing all buckets
    // ending at an aligned exclusiveEndMs as final, call sealThrough(exclusiveEndMs).
    // Late accepted trades in that range bump revision metadata, never FLB1.
    void sealThrough(int64_t exclusiveEndMs);
    // Drop only behind the seal when the caller's lateness window closes.
    // A request beyond the seal is clamped. Dropped buckets cannot reappear.
    void dropBefore(int64_t exclusiveStartMs);
    std::optional<uint32_t> revisionAt(int64_t bucketStartMs) const;
    uint64_t rejectedCount() const { return rejectedCount_; }
    // Subset of rejectedCount caused by the drop watermark.
    uint64_t droppedRejectCount() const { return droppedRejectCount_; }
    void clear();
private:
    struct AcceptedTrade { Trade trade; int64_t priceUnits = 0, baseUnits = 0, quoteUnits = 0, row = 0; };
    struct Bucket {
        std::vector<AcceptedTrade> trades; // sorted by exchange time and id at add.
        int64_t minRow = 0, maxRow = 0;
        int64_t baseTotal = 0, quoteTotal = 0;
        bool dirty = true;
        uint32_t revision = 0;
        FlowBar cached;
    };
    FlowBar materialize(int64_t startMs, Bucket& bucket) const;
    int64_t timeframeMs_;
    double nativeTick_;
    int64_t priceScale_, baseScale_, quoteScale_, rowTickUnits_;
    uint64_t rejectedCount_ = 0;
    uint64_t droppedRejectCount_ = 0;
    int64_t sealedThrough_ = INT64_MIN, droppedBefore_ = INT64_MIN;
    std::map<int64_t, Bucket> buckets_;
    std::deque<std::string> recentIds_;
    std::unordered_set<std::string> idSet_;
};
} // namespace sentinel::flow
