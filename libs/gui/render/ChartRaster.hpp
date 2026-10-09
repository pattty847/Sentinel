// Whole-pixel chart mapping (owner decision 2026-10-08, docs/research/2026-10-08-whole-pixel-mapping.md).
//
// The continuous camera is what GridViewState stores (exact, never rounded back from
// pixels). The raster camera is what the chart draws: an integer number of device
// pixels per heatmap row (P) and per column (C), row and column edges on device
// pixel edges, the zoom anchor kept within half a device pixel, and a drag applied
// as whole device pixels. computeRaster() is pure arithmetic (no allocation, any
// thread): the render thread runs it once per frame, the GUI thread evaluates it for
// the axis models, hit tests and the pan commit with the same inputs.
#pragma once

#include "TimeAxisMapping.hpp"

#include <QPointF>
#include <cstdint>

namespace chart_raster {

// A step to the next integer happens only this far (device px) beyond the
// half-integer: 2 -> 3 at r > 2.6, 3 -> 2 at r < 2.4. Owner default, no setting.
inline constexpr double kStepBandPx = 0.1;
// Larger cells are clamped (a direct viewport may ask for any zoom; the zoom
// floors keep real views far below this).
inline constexpr int kMaxCellPx = 1 << 20;

struct RasterInputs {
    int64_t timeStart = 0, timeEnd = 0; // the committed (continuous) view
    double minPrice = 0, maxPrice = 0;
    QPointF dragLogicalPx;              // an active drag's offset (0 when not dragging)
    double tfMs = 0, tick = 0;          // tick 0: none drawn yet (rows of one device pixel)
    double itemWidthLogical = 0, itemHeightLogical = 0, dpr = 1;
    // The view point that stays put across an integer step: fraction of the width
    // from the left, of the height from the top.
    double anchorFracX = 0.5, anchorFracY = 0.5;
};

// Last frame's integers (0: none yet).
struct RasterStep {
    int rowPx = 0, colPx = 0;
    bool operator==(const RasterStep &) const = default;
};

struct RasterCamera {
    bool valid = false;
    int rowPx = 0, colPx = 0; // P and C: device pixels per row and per column
    double dpr = 1;
    int widthDev = 0, heightDev = 0;
    int64_t topRowIndex = 0;  // n: the top edge is n * tick / P
    int64_t leftColIndex = 0; // m: the left edge is m * tf / C
    double tfMs = 0, tick = 0;
    // The drawn window (exactly what is on screen).
    double drawnStartMs = 0, drawnEndMs = 0, drawnMinPrice = 0, drawnMaxPrice = 0;
    // Item-relative device pixels.
    double xDev(double timeMs) const { return timeMs * colPx / tfMs - double(leftColIndex); }
    double yDev(double price) const { return double(topRowIndex) - price * rowPx / tick; }
    double timeAtXDev(double x) const { return (x + double(leftColIndex)) * tfMs / colPx; }
    double priceAtYDev(double y) const { return (double(topRowIndex) - y) * tick / rowPx; }
    RasterStep step() const { return {rowPx, colPx}; }
};

// Whole device pixels covered by a logical length (layout sizes are exact, so a
// product a hair under an integer still counts it).
int devicePixels(double logical, double dpr);
// The integer step rule: keep `previous` while |r - previous| <= 0.5 + kStepBandPx,
// else round (at least 1, at most kMaxCellPx).
int stepPixels(double r, int previous);

RasterCamera computeRaster(const RasterInputs &in, RasterStep previous);
// The frame's TimeAxisMapping (every layer maps through it): the drawn window over
// the device-integer surface, cellW = C / dpr, cellH = P / dpr.
TimeAxisMapping toMapping(const RasterCamera &camera);

// The committed shifts for a drag that shows `dragLogicalPx`: the camera moved by
// whole device pixels (llround(drag * dpr)) at its P and C. The time shift is whole
// ms, chosen so the drawn columns after the commit are exactly the dragged ones;
// the price shift keeps the drawn rows. False when the camera is invalid.
bool panShift(const RasterInputs &committed, RasterStep previous, QPointF dragLogicalPx, int64_t &timeShiftMs,
              double &priceShift);

// A price fit on whole pixels per row: [lo, hi] widened about its centre to
// heightDev * tick / P with P = max(1, floor(heightDev * tick / (hi - lo))), its top
// on a row edge of that P (the drawn window then equals the stored one). False
// (nothing changed) without a tick or a valid range.
bool fitPriceToRows(double &lo, double &hi, int heightDev, double tick);

} // namespace chart_raster
