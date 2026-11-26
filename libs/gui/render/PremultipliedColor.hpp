#pragma once
// Vertex colours for QSGVertexColorMaterial. That material blends premultiplied
// RGBA (One, OneMinusSrcAlpha): each channel must already be scaled by alpha.
// A straight colour with alpha < 255 adds its full RGB to what is underneath,
// so a translucent fill draws too bright (additive over the heatmap).
#include <QColor>
#include <QSGGeometry>
#include <algorithm>

namespace premultiplied {
struct Rgba { uchar r, g, b, a; };

// round(channel * alpha / 255); alpha 255 leaves the channel unchanged.
constexpr uchar scale(int channel, int alpha) {
    return uchar((std::clamp(channel, 0, 255) * std::clamp(alpha, 0, 255) + 127) / 255);
}
constexpr Rgba rgba(int r, int g, int b, int a) {
    return {scale(r, a), scale(g, a), scale(b, a), uchar(std::clamp(a, 0, 255))};
}
// The colour's RGB at `alpha`; the colour's own alpha is ignored.
inline Rgba rgba(const QColor& colour, int alpha) {
    return rgba(colour.red(), colour.green(), colour.blue(), alpha);
}
inline Rgba rgba(const QColor& colour) { return rgba(colour, colour.alpha()); }

inline void set(QSGGeometry::ColoredPoint2D& vertex, float x, float y, Rgba c) {
    vertex.set(x, y, c.r, c.g, c.b, c.a);
}
} // namespace premultiplied
