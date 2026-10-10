#pragma once
// Screen-space candle strokes. Vertices lie on device-pixel boundaries; the
// centre of an odd-width stroke lies on a device-pixel centre.
#include <algorithm>
#include <cmath>
#include <QSGGeometry>
#include "ChartRaster.hpp"

namespace candle_pixels {
inline constexpr double kCandleMaxBodyLogicalPx = 9.0;
struct Span { float lo, hi; };
inline int capacityFor(int current, int required) {
    int capacity = std::max(6, current);
    while (capacity < required) capacity *= 2;
    return capacity;
}
inline void setGeometryCount(QSGGeometry& geometry, int& capacity, int count) {
    if (count > capacity) {
        capacity = capacityFor(capacity, count);
        geometry.allocate(capacity);
    }
    geometry.setVertexCount(count);
}
inline unsigned char bodyAlpha(double opacity) {
    return static_cast<unsigned char>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255.0));
}
inline Span stroke(double centreLogical, int widthDevicePx, double dpr) {
    const double scale = std::isfinite(dpr) && dpr > 0 ? dpr : 1.0;
    const int width = std::clamp(widthDevicePx, 1, 3);
    const double left = std::floor(centreLogical * scale - width * 0.5 + 0.5);
    return {float(left / scale), float((left + width) / scale)};
}
inline Span doji(double yLogical, double dpr) {
    const double scale = std::isfinite(dpr) && dpr > 0 ? dpr : 1.0;
    const double top = std::floor(yLogical * scale);
    return {float(top / scale), float((top + 1.0) / scale)};
}
// A2 composes time and price independently. A free camera can still have one
// exact rest axis (or both while holding its end), so classify each axis from
// its continuous form and the corresponding whole-pixel origin/scale.
struct AxisSnap {
    bool x = true, y = true;
    bool operator==(const AxisSnap&) const = default;
};
inline AxisSnap axisSnap(const chart_raster::RasterCamera& camera) {
    return {camera.colPxF == camera.colPx && camera.leftF == camera.leftColIndex,
            camera.rowPxF == camera.rowPx && camera.topF == camera.topRowIndex};
}
struct Body { Span x, y; };
// Logical coordinates from the frame mapping. Snap only an axis that matches
// its whole-pixel camera; the moving axis keeps continuous edges.
inline Body body(double columnLeft, double columnRight, double openY, double closeY,
                 double dpr, bool snapX = true, bool snapY = true) {
    const double scale = std::isfinite(dpr) && dpr > 0 ? dpr : 1.0;
    double left = columnLeft * scale, right = columnRight * scale;
    double top = std::min(openY, closeY) * scale;
    double bottom = std::max(openY, closeY) * scale;
    if (!snapX) {
        const double width = std::max(1.0, std::min(right - left - 2.0, kCandleMaxBodyLogicalPx * scale));
        const double mid = (left + right) * 0.5;
        left = mid - width * 0.5;
        right = mid + width * 0.5;
    } else {
        left = std::round(left);
        right = std::round(right);
        const double columns = std::max(1.0, right - left);
        const double gap = columns >= 3 ? 1.0 : 0.0;
        const double maxBody = std::max(1L, std::lround(kCandleMaxBodyLogicalPx * scale));
        const double width = std::max(1.0, std::min(columns - 2.0 * gap, maxBody));
        // Opposite column/body parity has two equally close whole-pixel
        // positions. Choose the left one while keeping the requested width.
        left += std::floor((columns - width) * 0.5);
        right = left + width;
    }
    if (!snapY) {
        const double minHeight = openY == closeY ? 1.0 : std::max(1.0, 1.5 * scale);
        if (bottom - top < minHeight) {
            const double midY = (top + bottom) * 0.5;
            top = midY - minHeight * 0.5;
            bottom = midY + minHeight * 0.5;
        }
    } else if (openY == closeY) {
        const auto span = doji(openY, scale);
        top = span.lo * scale;
        bottom = span.hi * scale;
    } else {
        // Mapping arithmetic can put an exact bin edge a few ulps either
        // side of its integer. Preserve that edge before floor/ceil.
        const auto exactEdge = [](double v) {
            return std::abs(v - std::round(v)) < 1e-7 ? std::round(v) : v;
        };
        top = std::floor(exactEdge(top));
        bottom = std::ceil(exactEdge(bottom));
        const double minHeight = std::max(1L, std::lround(1.5 * scale));
        if (bottom - top < minHeight) {
            top = std::floor((top + bottom - minHeight) * 0.5);
            bottom = top + minHeight;
        }
    }
    return {{float(left / scale), float(right / scale)},
            {float(top / scale), float(bottom / scale)}};
}
} // namespace candle_pixels
