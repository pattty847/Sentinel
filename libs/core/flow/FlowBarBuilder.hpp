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
    // are integer volume units per base and quote currency. Rejected prints are
    // counted and never abort a journal build.
    FlowBarBuilder(int64_t timeframeMs, double nativeTick,
                   int64_t priceScale, int64_t baseScale, int64_t quoteScale);
    AddResult add(const Trade& trade);
    std::optional<FlowBar> bar(int64_t bucketStartMs);
    std::vector<FlowBar> bars();
    // Final publication is explicit: later accepted trades bump that bar's revision.
    void sealThrough(int64_t exclusiveEndMs);
    // Caller drops bars only after its lateness window closes.
    void dropBefore(int64_t exclusiveStartMs);
    uint64_t rejectedCount() const { return rejectedCount_; }
    void clear();
private:
    struct AcceptedTrade { Trade trade; int64_t priceUnits = 0, baseUnits = 0, quoteUnits = 0, row = 0; };
    struct Bucket {
        std::vector<AcceptedTrade> trades; // sorted by exchange time and id at add.
        int64_t minRow = 0, maxRow = 0;
        int64_t baseTotal = 0, quoteTotal = 0;
        bool dirty = true, finalPublished = false;
        uint32_t revision = 0;
        FlowBar cached;
    };
    FlowBar materialize(int64_t startMs, Bucket& bucket);
    int64_t timeframeMs_;
    double nativeTick_;
    int64_t priceScale_, baseScale_, quoteScale_, rowTickUnits_;
    uint64_t rejectedCount_ = 0;
    std::map<int64_t, Bucket> buckets_;
    std::deque<std::string> recentIds_;
    std::unordered_set<std::string> idSet_;
};
} // namespace sentinel::flow
