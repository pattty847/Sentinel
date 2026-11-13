#include "chart/layers/VolumeBarLayer.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include "render/CandlePixelGeometry.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

namespace chart {
namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
bool usable(const CandleOverlayBar& bar) {
    return std::isfinite(bar.volume) && bar.volume > 0;
}
void quad(Vertex*& out, const QRectF& rect, const QColor& color, double opacity) {
    const auto alpha = candle_pixels::bodyAlpha(opacity);
    const uchar r = color.red() * alpha / 255;
    const uchar g = color.green() * alpha / 255;
    const uchar b = color.blue() * alpha / 255;
    for (const auto p : {rect.topLeft(), rect.bottomLeft(), rect.topRight(),
                         rect.topRight(), rect.bottomLeft(), rect.bottomRight()})
        (out++)->set(float(p.x()), float(p.y()), r, g, b, alpha);
}
}
VolumeBarLayer::VolumeBarLayer() = default;
VolumeBarLayer::~VolumeBarLayer() = default;
void VolumeBarLayer::onRootRebuilt() { node_ = nullptr; capacity_ = 0; dirty_ = true; }
QSGNode* VolumeBarLayer::createContainer() {
    node_ = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node_->setGeometry(geometry); node_->setFlag(QSGNode::OwnsGeometry);
    node_->setMaterial(new QSGVertexColorMaterial); node_->setFlag(QSGNode::OwnsMaterial);
    return node_;
}
void VolumeBarLayer::capture(const MappingFrameContext& frame, std::span<const CandleOverlayBar> bars,
                             const Style& style) {
    frame_ = frame; style_ = style;
    bars_.assign(bars.begin(), bars.end());
    dirty_ = true;
    render();
}
void VolumeBarLayer::setBottomInset(double inset) {
    if (inset_ == inset) return;
    inset_ = inset; dirty_ = true;
    render();
}
void VolumeBarLayer::clear() {
    bars_.clear(); dirty_ = true;
    render();
}
void VolumeBarLayer::render() {
    if (!node_ || !dirty_) return;
    dirty_ = false;
    double maximum = 0;
    int count = 0;
    for (const auto& bar : bars_) {
        if (!usable(bar)) continue;
        maximum = std::max(maximum, bar.volume); ++count;
    }
    auto* geometry = node_->geometry();
    const double height = std::max(0.0, frame_.surfaceBounds.height() - inset_);
    if (!frame_.viewportValid || !frame_.mapping.valid || height <= 0) count = 0;
    candle_pixels::setGeometryCount(*geometry, capacity_, count * 6);
    auto* out = geometry->vertexDataAsColoredPoint2D();
    const auto snap = candle_pixels::axisSnap(frame_.raster);
    const double bottom = frame_.surfaceBounds.top() + height;
    if (count) for (const auto& bar : bars_) {
        if (!usable(bar)) continue;
        const auto left = frame_.mapping.timeToScreenX(bar.timeStartMs);
        const auto right = frame_.mapping.timeToScreenX(bar.timeStartMs + frame_.mapping.appendMs);
        const auto x = candle_pixels::body(left, right, 0, 0, frame_.surfaceDpr,
                                          snap.x, true, style_.fraction, style_.maxBodyPx).x;
        const double h = bar.volume / maximum * height * style_.bandPercent / 100.0;
        quad(out, QRectF(x.lo, bottom - h, x.hi - x.lo, h),
             bar.close >= bar.open ? style_.up : style_.down, style_.opacity);
    }
    node_->markDirty(QSGNode::DirtyGeometry);
}
} // namespace chart
