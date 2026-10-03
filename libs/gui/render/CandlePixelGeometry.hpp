#pragma once
// Screen-space candle strokes. Vertices lie on device-pixel boundaries; the
// centre of an odd-width stroke lies on a device-pixel centre.
#include <algorithm>
#include <cmath>
#include <QSGGeometry>

namespace candle_pixels {
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
} // namespace candle_pixels
