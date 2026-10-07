#include "UgrFrameMath.hpp"


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

} // namespace UgrFrameMath
