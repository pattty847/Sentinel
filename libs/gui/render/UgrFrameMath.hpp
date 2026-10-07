#pragma once

#include <QPointF>
#include <QRectF>

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

// The drag offset moves heatmap and overlays together while a pan is live.
ViewportState applyDragPan(ViewportState viewport, const QRectF& bounds);

} // namespace UgrFrameMath
