#include "ChartRaster.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace chart_raster {

int devicePixels(double logical, double dpr) {
    const double device = logical * dpr;
    if (!std::isfinite(device) || !(device > 0)) return 0;
    return static_cast<int>(std::min(std::floor(device + 1e-6), double(1 << 30)));
}

int stepPixels(double r, int previous) {
    if (!std::isfinite(r) || !(r > 0)) return 1;
    if (previous > 0 && std::abs(r - double(previous)) <= 0.5 + kStepBandPx) return previous;
    return static_cast<int>(std::clamp(std::round(r), 1.0, double(kMaxCellPx)));
}

int maxColumnPixels(double tfMs) {
    return static_cast<int>(std::clamp(std::floor(tfMs), 1.0, double(kMaxCellPx)));
}

RasterCamera computeRaster(const RasterInputs &in, RasterStep previous) {
    RasterCamera cam;
    cam.dpr = std::isfinite(in.dpr) && in.dpr > 0 ? in.dpr : 1.0;
    cam.widthDev = devicePixels(in.itemWidthLogical, cam.dpr);
    cam.heightDev = devicePixels(in.itemHeightLogical, cam.dpr);
    cam.tfMs = in.tfMs;
    const double timeSpan = double(in.timeEnd - in.timeStart);
    const double priceSpan = in.maxPrice - in.minPrice;
    if (!(in.tfMs > 0) || !(timeSpan > 0) || !std::isfinite(priceSpan) || !(priceSpan > 0) ||
        !std::isfinite(in.maxPrice) || cam.widthDev <= 0 || cam.heightDev <= 0)
        return cam;
    // Before a tick is drawn the rows are one device pixel tall (the mapping stays valid).
    const double tick = std::isfinite(in.tick) && in.tick > 0 ? in.tick : priceSpan / double(cam.heightDev);
    cam.tick = tick;
    const double rRow = double(cam.heightDev) * tick / priceSpan;
    const double rCol = double(cam.widthDev) * in.tfMs / timeSpan;
    cam.rowPx = stepPixels(rRow, previous.rowPx);
    // At least one ms per device pixel (a column of tf ms is at most tf px): a drag of
    // whole device pixels is then always a whole-ms commit (panShift). A stored window
    // narrower than that (a direct viewport of a few ms) draws wider than stored.
    cam.colPx = std::min(stepPixels(rCol, previous.colPx), maxColumnPixels(in.tfMs));
    const double P = cam.rowPx, C = cam.colPx;
    // The anchor keeps its screen position within half a device pixel for any P
    // and C; the drag moves the picture by whole device pixels, 1:1 with the mouse.
    const double fracX = std::isfinite(in.anchorFracX) ? std::clamp(in.anchorFracX, 0.0, 1.0) : 0.5;
    const double fracY = std::isfinite(in.anchorFracY) ? std::clamp(in.anchorFracY, 0.0, 1.0) : 0.5;
    const double anchorTime = double(in.timeStart) + fracX * timeSpan;
    const double anchorPrice = in.maxPrice - fracY * priceSpan;
    const double dragX = std::isfinite(in.dragLogicalPx.x()) ? in.dragLogicalPx.x() * cam.dpr : 0.0;
    const double dragY = std::isfinite(in.dragLogicalPx.y()) ? in.dragLogicalPx.y() * cam.dpr : 0.0;
    cam.leftColIndex = std::llround(anchorTime * C / in.tfMs - fracX * double(cam.widthDev)) - std::llround(dragX);
    cam.topRowIndex = std::llround(fracY * double(cam.heightDev) + anchorPrice * P / tick) + std::llround(dragY);
    cam.drawnStartMs = double(cam.leftColIndex) * in.tfMs / C;
    cam.drawnEndMs = cam.drawnStartMs + double(cam.widthDev) * in.tfMs / C;
    cam.drawnMaxPrice = double(cam.topRowIndex) * tick / P;
    cam.drawnMinPrice = cam.drawnMaxPrice - double(cam.heightDev) * tick / P;
    cam.rowPxF = P;
    cam.colPxF = C;
    cam.topF = double(cam.topRowIndex);
    cam.leftF = double(cam.leftColIndex);
    cam.valid = std::isfinite(cam.drawnStartMs) && std::isfinite(cam.drawnEndMs) && std::isfinite(cam.drawnMinPrice) &&
                std::isfinite(cam.drawnMaxPrice) && cam.drawnEndMs > cam.drawnStartMs &&
                cam.drawnMaxPrice > cam.drawnMinPrice;
    return cam;
}

namespace {
// The drawn window and validity of a free camera from its continuous form.
void finishFree(RasterCamera &cam) {
    cam.free = true;
    cam.drawnStartMs = cam.leftF * cam.tfMs / cam.colPxF;
    cam.drawnEndMs = cam.drawnStartMs + double(cam.widthDev) * cam.tfMs / cam.colPxF;
    cam.drawnMaxPrice = cam.topF * cam.tick / cam.rowPxF;
    cam.drawnMinPrice = cam.drawnMaxPrice - double(cam.heightDev) * cam.tick / cam.rowPxF;
    cam.valid = std::isfinite(cam.drawnStartMs) && std::isfinite(cam.drawnEndMs) && std::isfinite(cam.drawnMinPrice) &&
                std::isfinite(cam.drawnMaxPrice) && cam.drawnEndMs > cam.drawnStartMs &&
                cam.drawnMaxPrice > cam.drawnMinPrice && cam.rowPxF > 0 && cam.colPxF > 0;
}
} // namespace

RasterCamera continuousRaster(const RasterInputs &in) {
    RasterCamera cam = computeRaster(in, {}); // the dimensions, tick and the nearest rest integers
    if (!cam.valid) return cam;
    const double timeSpan = double(in.timeEnd - in.timeStart), priceSpan = in.maxPrice - in.minPrice;
    cam.rowPxF = double(cam.heightDev) * cam.tick / priceSpan;
    cam.colPxF = std::min(double(cam.widthDev) * in.tfMs / timeSpan, double(maxColumnPixels(in.tfMs)));
    const double fracX = std::isfinite(in.anchorFracX) ? std::clamp(in.anchorFracX, 0.0, 1.0) : 0.5;
    const double fracY = std::isfinite(in.anchorFracY) ? std::clamp(in.anchorFracY, 0.0, 1.0) : 0.5;
    const double anchorTime = double(in.timeStart) + fracX * timeSpan;
    const double anchorPrice = in.maxPrice - fracY * priceSpan;
    const double dragX = std::isfinite(in.dragLogicalPx.x()) ? double(std::llround(in.dragLogicalPx.x() * cam.dpr)) : 0;
    const double dragY = std::isfinite(in.dragLogicalPx.y()) ? double(std::llround(in.dragLogicalPx.y() * cam.dpr)) : 0;
    cam.leftF = anchorTime * cam.colPxF / in.tfMs - fracX * double(cam.widthDev) - dragX;
    cam.topF = fracY * double(cam.heightDev) + anchorPrice * cam.rowPxF / cam.tick + dragY;
    finishFree(cam);
    return cam;
}

RasterCamera onSurface(RasterCamera cam, double dpr, int widthDev, int heightDev) {
    if (!cam.valid || !(dpr > 0) || !std::isfinite(dpr) || (dpr == cam.dpr && widthDev == cam.widthDev &&
                                                            heightDev == cam.heightDev))
        return cam;
    // Device pixels scale with the ratio; the logical picture stays where it is.
    const double k = dpr / cam.dpr;
    cam.colPxF *= k;
    cam.rowPxF *= k;
    cam.leftF *= k;
    cam.topF *= k;
    cam.dpr = dpr;
    cam.widthDev = widthDev;
    cam.heightDev = heightDev;
    finishFree(cam);
    return cam;
}

RasterCamera composeAxes(const RasterCamera &timeCam, bool timeFree, const RasterCamera &priceCam, bool priceFree,
                         const RasterCamera &rest) {
    if (!rest.valid || (!timeFree && !priceFree)) return rest;
    RasterCamera cam = rest; // dimensions, tick, timeframe, the rest integers
    if (timeFree && timeCam.valid) {
        const RasterCamera t = onSurface(timeCam, rest.dpr, rest.widthDev, rest.heightDev);
        cam.colPxF = t.colPxF;
        cam.leftF = t.leftF;
    }
    if (priceFree && priceCam.valid) {
        // The same world scale at the frame's tick (the drawn rows are the frame's).
        const RasterCamera p = onSurface(priceCam, rest.dpr, rest.widthDev, rest.heightDev);
        cam.rowPxF = p.pxPerPrice() * rest.tick;
        cam.topF = p.topF;
    }
    finishFree(cam);
    return cam.valid ? cam : rest;
}

bool sameTimeAxis(const RasterCamera &a, const RasterCamera &b) {
    return a.valid && b.valid && a.colPx == b.colPx && a.leftColIndex == b.leftColIndex && a.widthDev == b.widthDev &&
           a.dpr == b.dpr && a.tfMs == b.tfMs;
}

bool samePriceAxis(const RasterCamera &a, const RasterCamera &b) {
    return a.valid && b.valid && a.tick == b.tick && a.rowPx == b.rowPx && a.topRowIndex == b.topRowIndex &&
           a.heightDev == b.heightDev && a.dpr == b.dpr;
}

RasterCamera glideRaster(const RasterCamera &start, const RasterCamera &to, double anchorTimeMs, double anchorPrice,
                         double e) {
    if (!(e < 1.0) || !start.valid || !to.valid) return to;
    e = std::max(0.0, e);
    // A start on another surface (a move to a screen of another device pixel ratio
    // mid-glide): its device coordinates in the target's, so the anchor keeps its
    // logical position.
    const RasterCamera from = onSurface(start, to.dpr, to.widthDev, to.heightDev);
    if (!from.valid) return to;
    RasterCamera cam = to;
    const double sx0 = from.pxPerMs(), sx1 = to.pxPerMs(), sy0 = from.pxPerPrice(), sy1 = to.pxPerPrice();
    const double sx = std::exp(std::log(sx0) * (1.0 - e) + std::log(sx1) * e);
    const double sy = std::exp(std::log(sy0) * (1.0 - e) + std::log(sy1) * e);
    const double ax = from.xDev(anchorTimeMs) + (to.xDev(anchorTimeMs) - from.xDev(anchorTimeMs)) * e;
    const double ay = from.yDev(anchorPrice) + (to.yDev(anchorPrice) - from.yDev(anchorPrice)) * e;
    cam.colPxF = sx * to.tfMs;
    cam.rowPxF = sy * to.tick;
    cam.leftF = anchorTimeMs * sx - ax;
    cam.topF = ay + anchorPrice * sy;
    finishFree(cam);
    return cam.valid ? cam : to;
}

RasterCamera shiftedRaster(RasterCamera cam, double dxDev, double dyDev) {
    if (!cam.valid || (dxDev == 0.0 && dyDev == 0.0)) return cam;
    cam.leftF -= dxDev;
    cam.topF += dyDev;
    if (cam.free) {
        finishFree(cam);
    } else {
        cam.leftColIndex -= std::llround(dxDev);
        cam.topRowIndex += std::llround(dyDev);
        cam.leftF = double(cam.leftColIndex);
        cam.topF = double(cam.topRowIndex);
        cam.drawnStartMs = double(cam.leftColIndex) * cam.tfMs / cam.colPx;
        cam.drawnEndMs = cam.drawnStartMs + double(cam.widthDev) * cam.tfMs / cam.colPx;
        cam.drawnMaxPrice = double(cam.topRowIndex) * cam.tick / cam.rowPx;
        cam.drawnMinPrice = cam.drawnMaxPrice - double(cam.heightDev) * cam.tick / cam.rowPx;
    }
    return cam;
}

TimeAxisMapping toMapping(const RasterCamera &cam) {
    TimeAxisMapping m;
    m.dpr = cam.dpr;
    m.rowPxDev = cam.rowPx;
    m.colPxDev = cam.colPx;
    if (!cam.valid) return m;
    const double tf = cam.tfMs, P = cam.rowPx, C = cam.colPx;
    m.viewStartMs = cam.drawnStartMs;
    m.viewEndMs = cam.drawnEndMs;
    m.viewMinPrice = cam.drawnMinPrice;
    m.viewMaxPrice = cam.drawnMaxPrice;
    // Columns are anchored to epoch multiples of the timeframe (spec rule 3). The
    // left column and the fraction of it cut by the edge come from the integer m
    // (exact; drawnStartMs carries ~1e-4 ms of rounding at epoch scale).
    const int64_t firstBucket =
        cam.leftColIndex >= 0 ? cam.leftColIndex / cam.colPx : -((-cam.leftColIndex + cam.colPx - 1) / cam.colPx);
    const double cutColumns = double(cam.leftColIndex - firstBucket * cam.colPx) / C;
    m.dataStartMs = double(firstBucket) * tf;
    m.dataEndMs = std::ceil(cam.drawnEndMs / tf) * tf;
    m.actualDataStartMs = cam.drawnStartMs;
    m.actualDataEndMs = cam.drawnEndMs;
    m.viewportColumns = true;
    m.dataMinPrice = cam.drawnMinPrice;
    m.dataMaxPrice = cam.drawnMaxPrice;
    m.appendMs = tf;
    m.tickSize = cam.tick;
    m.drawRect = QRectF(0.0, 0.0, double(cam.widthDev) / cam.dpr, double(cam.heightDev) / cam.dpr);
    m.srcRect = QRectF(cutColumns, 0.0, double(cam.widthDev) / C, double(cam.heightDev) / P);
    if (cam.free) {
        // A zoom transition: fractional columns and rows from the continuous form.
        const double columnsFromEpoch = cam.leftF / cam.colPxF;
        const double first = std::floor(columnsFromEpoch);
        m.dataStartMs = first * tf;
        m.srcRect = QRectF(columnsFromEpoch - first, 0.0, double(cam.widthDev) / cam.colPxF,
                           double(cam.heightDev) / cam.rowPxF);
    }
    m.gridWidth = static_cast<int>(std::ceil(m.srcRect.right())) + 1;
    m.gridHeight = static_cast<int>(std::ceil(m.srcRect.height()));
    m.filledColumns = m.gridWidth;
    m.timeOffset = 0.0f;
    m.cellW = (cam.free ? cam.colPxF : C) / cam.dpr;
    m.cellH = (cam.free ? cam.rowPxF : P) / cam.dpr;
    m.valid = true;
    return m;
}

bool panShift(const RasterInputs &committed, RasterStep previous, QPointF drag, int64_t &timeShiftMs,
              double &priceShift) {
    RasterInputs in = committed;
    in.dragLogicalPx = drag;
    const RasterCamera dragged = computeRaster(in, previous);
    if (!dragged.valid) return false;
    const double kx = std::isfinite(drag.x()) ? double(std::llround(drag.x() * dragged.dpr)) : 0.0;
    const double ky = std::isfinite(drag.y()) ? double(std::llround(drag.y() * dragged.dpr)) : 0.0;
    priceShift = ky * dragged.tick / dragged.rowPx;
    // Whole ms nearest the exact shift first, then its neighbours: the first that
    // keeps the dragged columns (a fraction of a ms is a fraction of a pixel).
    const double exact = -kx * dragged.tfMs / dragged.colPx;
    const int64_t nearest = std::llround(exact);
    in.dragLogicalPx = QPointF();
    in.minPrice = committed.minPrice + priceShift;
    in.maxPrice = committed.maxPrice + priceShift;
    for (const int64_t candidate : {nearest, nearest - 1, nearest + 1}) {
        in.timeStart = committed.timeStart + candidate;
        in.timeEnd = committed.timeEnd + candidate;
        const RasterCamera after = computeRaster(in, dragged.step());
        if (after.leftColIndex == dragged.leftColIndex && after.topRowIndex == dragged.topRowIndex) {
            timeShiftMs = candidate;
            return true;
        }
    }
    // No whole-ms commit reproduces the drawn columns. Unreachable while a column is
    // at most tf device px (computeRaster); never reported as an exact commit.
    return false;
}

double easeZoom(double t) {
    t = std::clamp(std::isfinite(t) ? t : 1.0, 0.0, 1.0);
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

namespace {
// One click's column rung (dir = +1 in, -1 out).
int columnRungOnce(int currentPx, int dir, int minPx, int maxPx) {
    const double aim = double(currentPx) * (dir > 0 ? kZoomStepRatio : 1.0 / kZoomStepRatio);
    int px = int(std::clamp(std::round(aim), 1.0, double(kMaxCellPx)));
    px = dir > 0 ? std::max(px, currentPx + 1) : std::min(px, currentPx - 1);
    if (minPx > 0) px = std::max(px, minPx);
    if (maxPx > 0) px = std::min(px, maxPx);
    // A limit in the click's direction: no step back past where it is.
    if ((dir > 0 && px < currentPx) || (dir < 0 && px > currentPx)) return currentPx;
    return std::max(px, 1);
}
} // namespace

int columnRung(int currentPx, int clicks, int minPx, int maxPx) {
    if (clicks == 0 || currentPx <= 0) return currentPx;
    // Batched clicks walk the ladder one click at a time (the same rungs as single
    // clicks), stopping at a limit.
    int px = currentPx;
    for (int i = 0; i < std::abs(clicks); ++i) {
        const int next = columnRungOnce(px, clicks > 0 ? 1 : -1, minPx, maxPx);
        if (next == px) break;
        px = next;
    }
    return px;
}

int nearestColumnRung(double colPxF, int minPx, int maxPx) {
    int px = int(std::clamp(std::isfinite(colPxF) ? std::round(colPxF) : 1.0, 1.0, double(kMaxCellPx)));
    if (minPx > 0) px = std::max(px, minPx);
    if (maxPx > 0) px = std::min(px, maxPx);
    return std::max(px, 1);
}

namespace {
bool sameTick(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b)); }
bool inLimits(double span, double minSpan, double maxSpan) {
    return (!(minSpan > 0) || span >= minSpan * (1 - 1e-12)) && (!(maxSpan > 0) || span <= maxSpan * (1 + 1e-12));
}
// The rung nearest `aim` (px per price) among whole px per row at the ticks the chart
// would draw them with; `accept` filters (direction). Empty when none qualifies.
RowRung bestRung(double aim, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt, double currentTick,
                 const std::function<bool(double scale)> &accept) {
    RowRung best;
    double bestScore = 1e300;
    double candidates[6] = {currentTick, tickAt(double(heightDev) / aim), 0, 0, 0, 0};
    int count = 2;
    // The ticks the neighbouring rungs would be drawn with (Auto's hysteresis).
    for (int pass = 0; pass < 2 && count < 6; ++pass) {
        const double t = candidates[pass];
        if (!(t > 0)) continue;
        for (const double p : {std::floor(aim * t), std::ceil(aim * t)})
            if (p >= 1 && count < 6) candidates[count++] = tickAt(double(heightDev) * t / p);
    }
    for (int i = 0; i < count; ++i) {
        const double tick = candidates[i];
        if (!(tick > 0) || !std::isfinite(tick)) continue;
        const double base = std::floor(aim * tick);
        for (const double p : {base - 1, base, base + 1, base + 2}) {
            if (p < 1 || p > double(kMaxCellPx)) continue;
            const double scale = p / tick, span = double(heightDev) * tick / p;
            if (!accept(scale) || !inLimits(span, minSpan, maxSpan) || !sameTick(tickAt(span), tick)) continue;
            const double score = std::abs(std::log(scale / aim));
            if (score < bestScore) {
                bestScore = score;
                best = {tick, int(p)};
            }
        }
    }
    return best;
}
} // namespace

namespace {
RowRung rowRungOnce(RowRung current, int clicks, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt) {
    const double s0 = double(current.rowPx) / current.tick;
    const double aim = s0 * std::pow(kZoomStepRatio, double(clicks));
    // Only the limit in the click's direction applies: a view beyond the other one
    // (e.g. an API view below the zoom-in floor) still zooms back toward it.
    const double lo = clicks > 0 ? minSpan : 0.0, hi = clicks < 0 ? maxSpan : 0.0;
    const RowRung best = bestRung(aim, heightDev, lo, hi, tickAt, current.tick, [&](double scale) {
        return clicks > 0 ? scale > s0 * (1 + 1e-9) : scale < s0 * (1 - 1e-9);
    });
    if (best.rowPx > 0) return best;
    // No whole-row rung the chart would keep in the limit: one px at the current tick,
    // or the rung at the limit, if that still moves.
    const double H = double(heightDev), t = current.tick;
    int px = current.rowPx + (clicks > 0 ? 1 : -1);
    if (clicks < 0 && hi > 0) px = std::max(px, int(std::ceil(H * t / hi - 1e-9)));
    if (clicks > 0 && lo > 0) px = std::min(px, int(std::floor(H * t / lo + 1e-9)));
    if (px >= 1 && (clicks > 0 ? px > current.rowPx : px < current.rowPx)) return {t, px};
    return current;
}
} // namespace

RowRung rowRung(RowRung current, int clicks, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt) {
    if (clicks == 0 || !(current.tick > 0) || current.rowPx <= 0 || heightDev <= 0) return current;
    // Batched clicks walk the ladder one click at a time, stopping at a limit.
    RowRung r = current;
    for (int i = 0; i < std::abs(clicks); ++i) {
        const RowRung next = rowRungOnce(r, clicks > 0 ? 1 : -1, heightDev, minSpan, maxSpan, tickAt);
        if (next == r) break;
        r = next;
    }
    return r;
}

RowRung nearestRowRung(double pxPerPrice, int heightDev, double minSpan, double maxSpan, const TickAt &tickAt) {
    if (!(pxPerPrice > 0) || !std::isfinite(pxPerPrice) || heightDev <= 0) return {};
    const RowRung best = bestRung(pxPerPrice, heightDev, minSpan, maxSpan, tickAt, tickAt(double(heightDev) / pxPerPrice),
                                  [](double) { return true; });
    if (best.rowPx > 0) return best;
    const double tick = tickAt(double(heightDev) / pxPerPrice);
    if (!(tick > 0)) return {};
    return {tick, std::max(1, int(std::lround(pxPerPrice * tick)))};
}

int64_t followShift(int64_t endMs, int64_t targetMs, int64_t tfMs, int colPx) {
    if (targetMs <= endMs) return 0;
    if (colPx <= 0 || tfMs <= 0) return targetMs - endMs;
    const int64_t slack = (tfMs + colPx - 1) / colPx; // one drawn pixel of time (ceil)
    if (targetMs - endMs <= slack) return 0;
    return (targetMs - endMs + tfMs - 1) / tfMs * tfMs;
}

bool fitPriceToRows(double &lo, double &hi, int heightDev, double tick) {
    const double span = hi - lo;
    if (heightDev <= 0 || !std::isfinite(tick) || !(tick > 0) || !std::isfinite(span) || !(span > 0) ||
        !std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(double(heightDev) * tick))
        return false;
    const double spanUlp = 4 * std::numeric_limits<double>::epsilon() * std::max(std::abs(lo), std::abs(hi));
    int p = int(std::clamp(std::floor(double(heightDev) * tick / std::max(span - spanUlp, span * .5)),
                          0.0, double(kMaxCellPx)));

    for (; p > 0; --p) {
        const double step = tick / p, span = double(heightDev) * tick / p;
        const double minTop = std::ceil(hi / step), maxTop = std::floor(lo / step + heightDev);
        if (minTop > maxTop) continue;
        const double top = std::clamp(std::round((lo + hi + span) * 0.5 / step), minTop, maxTop);
        // Outward rational edges preserve containment and candidate coverage
        // even at a fractional span (e.g. 640/3). The drawn camera still snaps
        // to these same integer pixel edges.
        auto edge = [&](double index, bool upper) {
            const double product = index * tick;
            const double productError = std::fma(index, tick, -product);
            double value = product / p;
            const double error = std::fma(value, double(p), -product) - productError;
            if (upper ? error < 0 : error > 0)
                value = std::nextafter(value, upper ? INFINITY : -INFINITY);
            return value;
        };
        const double fittedLo = edge(top - heightDev, false), fittedHi = edge(top, true);
        // Floating point at a large price/small tick must not crop an edge.
        if (fittedLo <= lo && fittedHi >= hi) {
            lo = fittedLo;
            hi = fittedHi;
            return true;
        }
    }
    return false;
}

std::optional<AutoPriceFit> solveAutoPriceFit(double lo, double hi, int heightDev, int64_t currentUnits,
                                             double priceScale, const heatmap::AutoTickParams &params,
                                             const BuildsPriceWindow &builds) {
    const double span = hi - lo;
    if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(span) || !(span > 0) || heightDev <= 0 ||
        !std::isfinite(priceScale) || !(priceScale > 0) || !builds)
        return std::nullopt;
    // Auto's hysteresis sees only the raw candle+margin span. Eligibility sees
    // each candidate's actual containing fit, including all newly exposed rows.
    const int64_t units = heatmap::autoTickUnitsIf(currentUnits, span * priceScale / heightDev,
        [&](int64_t candidate) {
            double low = lo, high = hi;
            return fitPriceToRows(low, high, heightDev, heatmap::fromUnits(candidate, priceScale)) &&
                   builds(candidate, low, high);
        }, params);
    if (units <= 0) return std::nullopt;
    const double tick = heatmap::fromUnits(units, priceScale);
    if (!fitPriceToRows(lo, hi, heightDev, tick)) return std::nullopt;
    return AutoPriceFit{lo, hi, tick, units, int(std::llround(heightDev * tick / (hi - lo)))};
}

} // namespace chart_raster
