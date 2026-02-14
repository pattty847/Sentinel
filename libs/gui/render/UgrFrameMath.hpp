#pragma once

#include <QPointF>
#include <QRectF>
#include <cstdint>
#include <limits>

namespace UgrFrameMath {

struct ViewportState {
    bool valid = false;
    double timeStart = 0.0;
    double timeEnd = 0.0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    QPointF panVisualOffset;
    bool dragging = false;
};

struct GridState {
    int gridWidth = 0;
    int gridHeight = 0;
    int64_t cadenceMs = 0;
    int64_t timeOriginMs = 0;
    int64_t lastSliceStartMs = std::numeric_limits<int64_t>::min();
    int filledColumns = 0;
    double tickSize = 0.0;
    double dataMinPrice = 0.0;
    double dataMaxPrice = 0.0;
    bool forceFull = false;
};

struct RenderRects {
    QRectF drawRect;
    QRectF srcRect;
    double viewTimeStart = 0.0;
    double viewTimeEnd = 0.0;
    double viewMinPrice = 0.0;
    double viewMaxPrice = 0.0;
    double dataStart = 0.0;
    double dataEnd = 0.0;
    double actualDataStart = 0.0;
    double actualDataEnd = 0.0;
    bool dataStartValid = false;
};

ViewportState applyDragPan(ViewportState viewport, const QRectF& bounds);
RenderRects computeRenderRects(const QRectF& bounds,
                               const ViewportState& viewport,
                               const GridState& grid);

} // namespace UgrFrameMath
