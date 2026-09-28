#include "UgrFrameMath.hpp"

#include <algorithm>

namespace UgrFrameMath {

ViewportState applyDragPan(ViewportState viewport, const QRectF& bounds) {
    const double timeRange = viewport.timeEnd - viewport.timeStart;
    const double priceRange = viewport.maxPrice - viewport.minPrice;
    if (!viewport.valid || viewport.panVisualOffset.isNull() ||
        bounds.width() <= 0.0 || bounds.height() <= 0.0 ||
        timeRange <= 0.0 || priceRange <= 0.0 || !viewport.dragging) {
        return viewport;
    }

    const double timePixelsToUnits = timeRange / bounds.width();
    const double pricePixelsToUnits = priceRange / bounds.height();
    const double timeDelta = -viewport.panVisualOffset.x() * timePixelsToUnits;
    const double priceDelta = viewport.panVisualOffset.y() * pricePixelsToUnits;
    viewport.timeStart += timeDelta;
    viewport.timeEnd += timeDelta;
    viewport.minPrice += priceDelta;
    viewport.maxPrice += priceDelta;
    return viewport;
}

RenderRects computeRenderRects(const QRectF& bounds,
                               const ViewportState& viewport,
                               const GridState& grid) {
    RenderRects out;
    out.drawRect = bounds;
    out.srcRect = QRectF(0, 0, grid.gridWidth, grid.gridHeight);
    out.viewTimeStart = viewport.timeStart;
    out.viewTimeEnd = viewport.timeEnd;
    out.viewMinPrice = viewport.minPrice;
    out.viewMaxPrice = viewport.maxPrice;

    if (grid.cadenceMs <= 0 || grid.gridWidth <= 0) {
        return out;
    }

    const int64_t bufferSpanMs = static_cast<int64_t>(grid.gridWidth) * grid.cadenceMs;
    out.dataEnd = (grid.lastSliceStartMs != std::numeric_limits<int64_t>::min() && bufferSpanMs > 0)
        ? static_cast<double>(grid.lastSliceStartMs + grid.cadenceMs)
        : static_cast<double>(grid.timeOriginMs + bufferSpanMs);
    out.dataStart = out.dataEnd - static_cast<double>(bufferSpanMs);
    out.dataStartValid = (out.dataEnd > out.dataStart);

    if (grid.filledColumns > 0) {
        const int64_t filledSpanMs = static_cast<int64_t>(grid.filledColumns) * grid.cadenceMs;
        out.actualDataEnd = out.dataEnd;
        out.actualDataStart = out.dataEnd - static_cast<double>(filledSpanMs);
    }

    const double viewTimeSpan = out.viewTimeEnd - out.viewTimeStart;
    const double viewPriceSpan = out.viewMaxPrice - out.viewMinPrice;
    if (grid.forceFull || !viewport.valid ||
        grid.tickSize <= 0.0 || grid.timeOriginMs == 0 ||
        viewTimeSpan <= 0.0 || viewPriceSpan <= 0.0) {
        return out;
    }

    const double overlapStart = std::max(out.viewTimeStart, out.dataStart);
    const double overlapEnd = std::min(out.viewTimeEnd, out.dataEnd);
    const double overlapMin = std::max(out.viewMinPrice, grid.dataMinPrice);
    const double overlapMax = std::min(out.viewMaxPrice, grid.dataMaxPrice);

    if (overlapEnd <= overlapStart || overlapMax <= overlapMin) {
        out.drawRect = QRectF();
        out.srcRect = QRectF();
        return out;
    }

    const double overlapTimeSpan = overlapEnd - overlapStart;
    const double overlapPriceSpan = overlapMax - overlapMin;
    const double timeRatioStart = (overlapStart - out.viewTimeStart) / viewTimeSpan;
    const double timeRatioEnd = (overlapEnd - out.viewTimeStart) / viewTimeSpan;
    const double priceRatioTop = (out.viewMaxPrice - overlapMax) / viewPriceSpan;
    const double priceRatioBottom = (out.viewMaxPrice - overlapMin) / viewPriceSpan;

    out.drawRect = QRectF(
        bounds.x() + bounds.width() * timeRatioStart,
        bounds.y() + bounds.height() * priceRatioTop,
        bounds.width() * (timeRatioEnd - timeRatioStart),
        bounds.height() * (priceRatioBottom - priceRatioTop));

    const double maxCoordX = static_cast<double>(grid.gridWidth);
    const double maxCoordY = static_cast<double>(grid.gridHeight);
    const double srcW = std::clamp(overlapTimeSpan / grid.cadenceMs, 1.0, maxCoordX);
    const double srcH = std::clamp(overlapPriceSpan / grid.tickSize, 1.0, maxCoordY);
    double srcX = (overlapStart - out.dataStart) / grid.cadenceMs;
    double srcY = (grid.dataMaxPrice - overlapMax) / grid.tickSize;
    srcX = std::clamp(srcX, 0.0, maxCoordX - srcW);
    srcY = std::clamp(srcY, 0.0, maxCoordY - srcH);
    out.srcRect = QRectF(srcX, srcY, srcW, srcH);
    return out;
}

} // namespace UgrFrameMath
