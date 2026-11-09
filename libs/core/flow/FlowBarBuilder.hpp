#pragma once
#include "FlowBar.hpp"
#include <deque>
#include <map>
#include <unordered_set>

namespace sentinel::flow {
// Accepts a bounded trade-id window; bars are materialized in exchange-time order.
class FlowBarBuilder {
public:
    FlowBarBuilder(int64_t timeframeMs, double nativeTick);
    bool add(const Trade& trade); // false for a recent duplicate id.
    std::vector<FlowBar> bars() const;
    void clear();
private:
    struct Bucket { std::vector<Trade> trades; int64_t minRow = 0, maxRow = 0; };
    int64_t timeframeMs_;
    double nativeTick_;
    std::map<int64_t, Bucket> buckets_;
    mutable std::unordered_set<int64_t> published_;
    std::map<int64_t, uint32_t> revisions_;
    std::deque<std::string> recentIds_;
    std::unordered_set<std::string> idSet_;
};
} // namespace sentinel::flow
