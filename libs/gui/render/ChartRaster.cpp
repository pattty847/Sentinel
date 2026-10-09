#include "ChartRaster.hpp"

#include <algorithm>
#include <cmath>

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
    // At most one ms per device pixel (a column of tf ms is at most tf px): a drag of
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
    cam.valid = std::isfinite(cam.drawnStartMs) && std::isfinite(cam.drawnEndMs) && std::isfinite(cam.drawnMinPrice) &&
                std::isfinite(cam.drawnMaxPrice) && cam.drawnEndMs > cam.drawnStartMs &&
                cam.drawnMaxPrice > cam.drawnMinPrice;
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
    m.gridWidth = static_cast<int>(std::ceil(m.srcRect.right())) + 1;
    m.gridHeight = static_cast<int>(std::ceil(m.srcRect.height()));
    m.filledColumns = m.gridWidth;
    m.timeOffset = 0.0f;
    m.cellW = C / cam.dpr;
    m.cellH = P / cam.dpr;
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

bool fitPriceToRows(double &lo, double &hi, int heightDev, double tick) {
    const double span = hi - lo;
    if (heightDev <= 0 || !std::isfinite(tick) || !(tick > 0) || !std::isfinite(span) || !(span > 0) ||
        !std::isfinite(lo) || !std::isfinite(hi))
        return false;
    const double rows = double(heightDev) * tick / span;
    const double P = std::clamp(std::floor(rows), 1.0, double(kMaxCellPx));
    const double fitted = double(heightDev) * tick / P;
    const double top = std::round(((lo + hi) * 0.5 + fitted * 0.5) * P / tick) * tick / P;
    if (!std::isfinite(top)) return false;
    hi = top;
    lo = top - fitted;
    return true;
}

} // namespace chart_raster
