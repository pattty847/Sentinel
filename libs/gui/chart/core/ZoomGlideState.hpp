#pragma once

#include "render/ChartRaster.hpp"

namespace chart {

// Time and price glide independently; the frame reads these values during sync.
struct AxisGlide {
    bool active = false;
    bool holding = false; // at its end, waiting for the frame's rest camera (a tick commit)
    // Full cameras without a drag; this axis's part is used. The end is fixed when the
    // glide starts: the stored view's rest camera at the tick it is drawn with.
    chart_raster::RasterCamera from, end;
    double anchorTime = 0, anchorPrice = 0; // the content under the cursor
    // beganMs: the click (or gesture end); a retarget never lands later than
    // beganMs + 2 * kZoomGlideMs.
    qint64 beganMs = 0, startMs = 0, deadlineMs = 0;
    double progress = 1.0; // eased, for the frame being prepared
};

struct ZoomGlideState {
    AxisGlide time, price;
    int rebases = 0; // diagnostics and tests
    // The rungs the glides go to: rapid clicks step on from here.
    int colPx = 0;
    chart_raster::RowRung row;
    bool gesture = false; // a pinch or trackpad scroll draws the continuous camera
    // The axes the gesture has zoomed (the others stay the rest camera's whole pixels).
    bool gestureTime = false, gesturePrice = false;
};

} // namespace chart
