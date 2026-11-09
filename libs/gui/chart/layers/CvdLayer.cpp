#include "chart/layers/CvdLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "render/ChartTextRenderer.hpp"
#include <QSGOpacityNode>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <cstdio>
#include <cstring>
namespace chart {
namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
constexpr int kVertices = CvdSeries::kCapacity * 18 + 12;
void triangle(Vertex*& out, QPointF a, QPointF b, QPointF c, QColor color) {
    const auto alpha = color.alpha();
    for (const auto p : {a, b, c}) {
        (out++)->set(float(p.x()), float(p.y()), uchar(color.red() * alpha / 255),
                    uchar(color.green() * alpha / 255), uchar(color.blue() * alpha / 255), uchar(alpha));
    }
}
void quad(Vertex*& out, const QRectF& rect, QColor color) {
    triangle(out, rect.topLeft(), rect.bottomLeft(), rect.topRight(), color);
    triangle(out, rect.topRight(), rect.bottomLeft(), rect.bottomRight(), color);
}
QColor signColor(double value) { return value >= 0 ? QColor(46, 189, 133) : QColor(246, 70, 93); }
void segment(Vertex*& out, QPointF a, QPointF b, double zero, bool area, double stroke) {
    const auto color = signColor(zero - (a.y() + b.y()) * 0.5);
    if (area) {
        auto fill = color; fill.setAlpha(110);
        triangle(out, a, {a.x(), zero}, b, fill);
        triangle(out, b, {a.x(), zero}, {b.x(), zero}, fill);
    } else {
        // A one-logical-pixel ribbon stays deterministic on every QSG backend.
        triangle(out, a, a + QPointF(0, stroke), b, color);
        triangle(out, b, a + QPointF(0, stroke), b + QPointF(0, stroke), color);
    }
}
void signedSegment(Vertex*& out, QPointF a, QPointF b, double zero, const QRectF& strip, bool area) {
    if (b.x() <= strip.left() || a.x() >= strip.right() || b.x() <= a.x()) return;
    const double slope = (b.y() - a.y()) / (b.x() - a.x());
    if (a.x() < strip.left()) a = {strip.left(), a.y() + (strip.left() - a.x()) * slope};
    if (b.x() > strip.right()) b = {strip.right(), b.y() + (strip.right() - b.x()) * slope};
    if ((a.y() < zero && b.y() > zero) || (a.y() > zero && b.y() < zero)) {
        const QPointF cross(a.x() + (b.x() - a.x()) * (zero - a.y()) / (b.y() - a.y()), zero);
        segment(out, a, cross, zero, area, std::min(1.0, strip.height()));
        segment(out, cross, b, zero, area, std::min(1.0, strip.height()));
    } else segment(out, a, b, zero, area, std::min(1.0, strip.height()));
}
}
CvdLayer::CvdLayer() : current_(std::make_unique<CvdSnapshot>()) { glyphs_.reserve(128); }
void CvdLayer::onRootRebuilt() {
    gate_ = nullptr; node_ = nullptr; vertices_ = 0; reserved_ = false; dirty_ = true;
}
QSGNode* CvdLayer::createContainer() {
    if (!captured_.stripPx) return nullptr;
    gate_ = new QSGOpacityNode;
    node_ = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), kVertices);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    std::memset(geometry->vertexData(), 0, size_t(kVertices) * sizeof(Vertex));
    node_->setGeometry(geometry); node_->setFlag(QSGNode::OwnsGeometry);
    node_->setMaterial(new QSGVertexColorMaterial); node_->setFlag(QSGNode::OwnsMaterial);
    gate_->appendChildNode(node_);
    return gate_;
}
void CvdLayer::sync(const ChartFrame& frame) {
    captured_ = presentation_;
    if (mailbox_.take(current_)) dirty_ = true;
    drawing_ = captured_.stripPx > 0 && frame.context.raster.valid && frame.context.raster.dpr > 0 &&
        current_->timeframeMs == std::max<int64_t>(1000, frame.context.time.activeTimeframeMs) && current_->reset == captured_.reset &&
        current_->series.count > 0;
}
void CvdLayer::geometry(const ChartFrame& frame) {
    auto* geom = node_->geometry();
    auto* begin = geom->vertexDataAsColoredPoint2D();
    auto* out = begin;
    quad(out, strip_, QColor(16, 20, 26, 220));
    scale_ = current_->series.scale(frame.context.raster.drawnStartMs, frame.context.raster.drawnEndMs, current_->timeframeMs);
    const double stroke = std::min(1.0, strip_.height());
    const double padding = std::min(2.0, (strip_.height() - stroke) * 0.25);
    const auto y = [&](double value) {
        return strip_.bottom() - stroke - padding - std::clamp(scale_.fraction(value), 0.0, 1.0) * (strip_.height() - stroke - 2 * padding);
    };
    const double zero = y(0);
    quad(out, QRectF(strip_.left(), zero, strip_.width(), stroke), QColor(160, 170, 180, 100));
    const auto& cam = frame.context.raster;
    const auto x = [&](double time) { return frame.context.surfaceBounds.left() + cam.xDev(time) / cam.dpr; };
    const auto& series = current_->series;
    for (int i = 0; i < series.count; ++i) {
        const auto& p = series.points[size_t(i)];
        const QPointF b(x(double(p.timeMs) + current_->timeframeMs * 0.5), y(p.value));
        const bool connected = i && series.points[size_t(i - 1)].resetMs == p.resetMs &&
            p.timeMs - series.points[size_t(i - 1)].timeMs == current_->timeframeMs;
        if (connected) {
            const auto& previous = series.points[size_t(i - 1)];
            signedSegment(out, {x(double(previous.timeMs) + current_->timeframeMs * 0.5), y(previous.value)}, b, zero, strip_, captured_.area);
        } else signedSegment(out, {x(double(p.timeMs)), b.y()}, b, zero, strip_, captured_.area);
    }
    const int used = int(out - begin);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geom->setVertexCount(used);
#else
    if (used < vertices_) std::memset(out, 0, size_t(vertices_ - used) * sizeof(Vertex));
#endif
    vertices_ = used; geom->markVertexDataDirty(); node_->markDirty(QSGNode::DirtyGeometry);
}
void CvdLayer::updateNodes(const ChartFrame& frame, LayerNodeContext&) {
    if (!gate_) return;
    strip_ = frame.context.surfaceBounds;
    const double height = std::min(double(captured_.stripPx), strip_.height());
    strip_.setTop(strip_.bottom() - height);
    gate_->setOpacity(drawing_ && !strip_.isEmpty() ? 1 : 0);
    if (!drawing_ || strip_.isEmpty()) return;
    const auto& cam = frame.context.raster;
    const Key key{cam.leftF, cam.colPxF, cam.tfMs, cam.dpr, cam.drawnStartMs, cam.drawnEndMs, strip_, captured_.area};
    if (!dirty_ && key == key_) return;
    key_ = key; dirty_ = false;
    geometry(frame);
}
void CvdLayer::addLabel(const char* text, const QRectF& box, const ChartTextAtlas& atlas) {
    if (box.height() < 12 || box.width() <= 0) return;
    const double scale = 12 / double(atlas.fontPx());
    double pen = box.left();
    for (const char* c = text; *c && glyphs_.size() < 128; ++c) {
        const auto& g = atlas.glyph(QChar(*c));
        if (pen + g.advance * scale > box.right()) break;
        ChartGlyphInstance glyph;
        glyph.rect = QRectF(pen + g.bounds.left() * scale, box.top() - atlas.glyphTopPx() * scale + g.bounds.top() * scale,
                            g.bounds.width() * scale, g.bounds.height() * scale);
        glyph.uv = g.uv; glyph.color = ChartLookSettings::footprintText();
        glyphs_.push_back(glyph); pen += g.advance * scale;
    }
}
void CvdLayer::submitLabels(const ChartFrame&, LayerNodeContext& context) {
    if (!drawing_ || !context.atlasReady || context.atlas.fontPx() <= 0) return;
    if (!reserved_) reserved_ = context.text.reserve(ChartLookSettings::footprintText(), 128);
    glyphs_.clear();
    const char* label = captured_.reset == CvdReset::None ? "CVD since loaded start" :
        captured_.reset == CvdReset::UtcDay ? "CVD since UTC day" : "CVD since TPO session";
    addLabel(label, {strip_.left() + 4, strip_.top() + 2, std::max(0.0, strip_.width() - 110), strip_.height() - 4}, context.atlas);
    char value[32];
    const double width = std::min(100.0, strip_.width() - 8);
    const double left = strip_.right() - width - 4;
    std::snprintf(value, sizeof(value), "%.6g", scale_.max);
    addLabel(value, {left, strip_.top() + 2, width, strip_.height() - 4}, context.atlas);
    std::snprintf(value, sizeof(value), "%.6g", scale_.min);
    if (strip_.height() >= 30) addLabel(value, {left, strip_.bottom() - 14, width, 12}, context.atlas);
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::High);
}
} // namespace chart
