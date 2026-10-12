#pragma once

#include "chart/core/ChartFrame.hpp"
#include "chart/core/PublishedFrame.hpp"

namespace chart {

// Render thread during sync. Inputs are captured values; no GUI-owned references.
class FrameProducer {
public:
    const ChartFrame& produce(const FrameInputs& inputs, ITickAuthority& ticks, PublishedFrame& published);
    ChartFrame& frame() { return frame_; }

private:
    void computeGpuFrameMapping(const FrameInputs& inputs, ITickAuthority& ticks, PublishedFrame& published);
    static void publishFrameContext(const FrameContext& frame, PublishedFrame& destination);
    struct RasterSurface {
        chart_raster::RasterStep step;
        double dpr = 0, tick = 0;
        int widthDev = 0, heightDev = 0;
        bool operator==(const RasterSurface&) const = default;
    } announced_;
    ChartFrame frame_;
};

} // namespace chart
