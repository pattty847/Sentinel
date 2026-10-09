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
#include <functional>

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
    // The camera's continuous form. At rest (free == false) these are exactly P, C, n
    // and m. During a zoom transition (a glide between rungs, a pinch) the camera is
    // free: fractional pixels per row and column and a fractional origin, drawn with
    // coverage in linear light; rowPx/colPx/topRowIndex/leftColIndex are then the
    // target rung's (rest) integers.
    bool free = false;
    double rowPxF = 0, colPxF = 0, topF = 0, leftF = 0;
    // Item-relative device pixels.
    double xDev(double timeMs) const { return timeMs * colPxF / tfMs - leftF; }
    double yDev(double price) const { return topF - price * rowPxF / tick; }
    double timeAtXDev(double x) const { return (x + leftF) * tfMs / colPxF; }
    double priceAtYDev(double y) const { return (topF - y) * tick / rowPxF; }
    double pxPerMs() const { return colPxF / tfMs; }
    double pxPerPrice() const { return rowPxF / tick; }
    RasterStep step() const { return {rowPx, colPx}; }
};

// Whole device pixels covered by a logical length (layout sizes are exact, so a
// product a hair under an integer still counts it).
int devicePixels(double logical, double dpr);
// The integer step rule: keep `previous` while |r - previous| <= 0.5 + kStepBandPx,
// else round (at least 1, at most kMaxCellPx).
int stepPixels(double r, int previous);
// Columns are at most tf device px (at least one ms per device pixel; at most kMaxCellPx): every
// whole-pixel drag then has an exact whole-ms commit.
int maxColumnPixels(double tfMs);

RasterCamera computeRaster(const RasterInputs &in, RasterStep previous);
// The continuous camera of the stored view (no rounding): free, fractional pixels per
// row and column, the anchor exactly at its fraction; a drag still whole pixels. A
// pinch draws it while the fingers move.
RasterCamera continuousRaster(const RasterInputs &in);
// A zoom glide between two cameras of the same surface and timeframe: the scales
// (px per ms, px per price) interpolate geometrically and the anchor point (world time
// and price) moves linearly from where `from` draws it to where `to` draws it, at
// eased progress e. e >= 1 returns `to` exactly (the landing frame is the rest frame).
RasterCamera glideRaster(const RasterCamera &from, const RasterCamera &to, double anchorTimeMs, double anchorPrice,
                         double e);
// A camera moved by whole device pixels (dx right, dy down), its free form included.
RasterCamera shiftedRaster(RasterCamera camera, double dxDev, double dyDev);
// The frame's TimeAxisMapping (every layer maps through it): the drawn window over
// the device-integer surface, cellW = C / dpr, cellH = P / dpr.
TimeAxisMapping toMapping(const RasterCamera &camera);

// The committed shifts for a drag that shows `dragLogicalPx`: the camera moved by
// whole device pixels (llround(drag * dpr)) at its P and C. The time shift is whole
// ms, chosen so the drawn columns after the commit are exactly the dragged ones;
// the price shift keeps the drawn rows. False when the camera is invalid or no
// whole-ms shift reproduces the dragged camera (never with maxColumnPixels).
bool panShift(const RasterInputs &committed, RasterStep previous, QPointF dragLogicalPx, int64_t &timeShiftMs,
              double &priceShift);

// ---------------------------------------------------------------- smooth zoom (A2)
// A wheel click moves to the next RUNG: a view with whole device pixels per column and
// per row (at the tick drawn there), reached by a short eased glide. kZoomStepRatio is
// the scale one click aims for; the rung is the whole-pixel size nearest that aim and
// always at least one pixel away from the current one (no dead click). Owner-tunable
// constant (no setting). Rationale: 1.25 is about a third of an octave, so a column of
// 16 px goes 16 -> 20 -> 25 -> 31 and 2 px rows 2 -> 3 -> 4 -> 5 -> 6 -> 8, a clear
// change per click that stays readable at small cells.
inline constexpr double kZoomStepRatio = 1.25;
// The glide to a rung: duration and easing (cubic ease-out: fast start, soft landing).
inline constexpr int kZoomGlideMs = 130;
double easeZoom(double t);
// The column rung `clicks` wheel clicks from `currentPx` (> 0 zooms in): the whole px
// width nearest currentPx * kZoomStepRatio^clicks, at least one px per direction away,
// inside [minPx, maxPx] (currentPx when the limit is reached).
int columnRung(int currentPx, int clicks, int minPx, int maxPx);
// The column rung nearest a continuous width (a pinch's end).
int nearestColumnRung(double colPxF, int minPx, int maxPx);
// A price rung: the tick drawn there and whole device px per row (span = heightDev *
// tick / rowPx). tickAt(span) predicts the tick the chart draws a price span with
// (Auto's rule and state, or the Manual tick); a rung is only one where the predicted
// tick is the rung's tick, so it lands on whole rows. minSpan/maxSpan <= 0: no limit;
// a click obeys only the limit in its direction (a zoom-out from below the zoom-in
// floor still moves).
struct RowRung {
    double tick = 0;
    int rowPx = 0;
    bool operator==(const RowRung &) const = default;
};
using TickAt = std::function<double(double priceSpan)>;
RowRung rowRung(RowRung current, int clicks, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt);
RowRung nearestRowRung(double pxPerPrice, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt);

// Follow-live: the shift that brings the view end to `targetMs` (the live bucket one
// padding inside): 0 while the end is within one drawn pixel (tf / colPx ms) of it or
// past it; else whole buckets (multiples of tf), so the drawn phase never changes and a
// live step moves the picture by whole columns. colPx <= 0: the exact shift.
int64_t followShift(int64_t endMs, int64_t targetMs, int64_t tfMs, int colPx);

// A price fit on whole pixels per row: [lo, hi] widened about its centre to
// heightDev * tick / P with P = max(1, floor(heightDev * tick / (hi - lo))), its top
// on a row edge of that P (the drawn window then equals the stored one). False
// (nothing changed) without a tick or a valid range.
bool fitPriceToRows(double &lo, double &hi, int heightDev, double tick);

} // namespace chart_raster
