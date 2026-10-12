#pragma once
#include <algorithm>
namespace heatmap::gpu {
// The label request retry: a posted request that got no result (or lost
// it to HeatmapCellQuery::cancel()) is asked again after delayMs(), which doubles
// per failure up to kMaxMs and returns to kFirstMs after a result.
struct LabelRetryPolicy {
    static constexpr int kFirstMs = 2000, kMaxMs = 30000;
    int delayMs() const { return delay_; }
    void failed() { delay_ = std::min(delay_ * 2, kMaxMs); }
    void succeeded() { delay_ = kFirstMs; }
private:
    int delay_ = kFirstMs;
};

} // namespace heatmap::gpu
