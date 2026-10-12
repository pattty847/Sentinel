#pragma once

#include "chart/core/ChartCamera.hpp"

namespace chart {

// Fixed-size values captured during sync while the GUI thread is blocked.
struct FrameInputs {
    FrameContext context;
    CameraInputs camera;
    int64_t timeframeMs = 0;
};

// Render-owned frame; consumers retain values, never references across syncs.
struct ChartFrame {
    FrameContext context;
    chart_raster::RasterCamera rest;
    bool drawHeatmap = false;
    float coverageGamma = 0.0f;
    std::optional<BinView> binView;
    bool rasterChanged = false;
    bool ready = false;
};

} // namespace chart
