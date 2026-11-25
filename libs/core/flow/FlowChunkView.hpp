#pragma once
// A disposable composed view. Decoded history belongs only to FlowChunkCache;
// composition runs on the flow data thread, never on the GUI/render threads.
#include "FlowChunkCache.hpp"
#include "FlowChunkPlan.hpp"
#include "FlowBar.hpp"
#include <map>
namespace sentinel::flow {
struct FlowViewBar {
    FlowBar bar;
    uint32_t revision = 0;
    bool complete = false;
    bool sealed = false;
};
struct FlowViewBucket {
    chunks::Status status = chunks::Status::Building;
    std::optional<FlowViewBar> value;
};
struct FlowViewSnapshot {
    std::map<int64_t, FlowViewBucket> buckets;
    // Summary delta before the first drawn bucket, since the caller's fixed
    // UTC-day/session anchor. Visible-reset mode supplies no anchor.
    int64_t anchorMs = -1, prefixThroughMs = 0, loadedFromMs = 0;
    double prefixDelta = 0;
    bool prefixComplete = false;
};
// One screen on each side of the drawn range, sharing the chart surface cap.
FlowChunkWant paddedFlowView(FlowChunkWant);
class FlowChunkView {
public:
    void resetLive();
    void live(FlowViewBar bar, chunks::Status status);
    void retention(int64_t liveFromMs);
    FlowViewSnapshot compose(FlowChunkCache&, const FlowChunkWant&,
                             const protocol::chunks::Availability&, int64_t anchorMs = -1);
private:
    std::map<int64_t, FlowViewBucket> live_;
    // A held sealed bucket retires its preview permanently within the retained
    // live window, including if a later epoch temporarily fences the chunk.
    std::map<int64_t, int64_t> retired_;
    int64_t liveFromMs_ = 0;
    // Scalar running prefix only: no second history body or retained prefix bars.
    struct Prefix {
        std::string instrument;
        int64_t anchor = -1, tf = 0, through = 0, loaded = 0;
        uint64_t generation = 0;
        double delta = 0;
        bool complete = false;
    } prefix_;

};
}
