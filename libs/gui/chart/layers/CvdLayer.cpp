#include "chart/layers/CvdLayer.hpp"
#include "chart/core/FlowNumberFormat.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "render/ChartTextRenderer.hpp"
#include "render/AxisTextService.hpp"
#include <QSGOpacityNode>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <cstdio>
#include <cstring>
namespace chart {
namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
constexpr int kVertices = CvdSeries::kCapacity * 18 + 18;
struct Ink { bool area; int edgeSign; double stroke; double top; double bottom; };
int edgeSign(const CvdGutterValues& values, CvdFillAnchor anchor) {
    if (!values.valid || (anchor != CvdFillAnchor::Auto && anchor != CvdFillAnchor::Edge)) return 0;
    if (values.max < 0) return -1;
    if (values.min > 0) return 1;
    return 0;
}
CvdScale plotScale(CvdScale zeroScale, const CvdGutterValues& values, bool area, bool edge) {
    if (!area || !edge || !values.valid) return zeroScale;
    CvdScale scale{values.min, values.max};
    if (scale.min == scale.max) {
        const double halfSpan = std::max(std::abs(scale.min) * 0.01, 1e-9);
        scale.min -= halfSpan; scale.max += halfSpan;
    }
    return scale;
}
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
void segment(Vertex*& out, QPointF a, QPointF b, double zero, Ink ink) {
    const bool positive = ink.edgeSign ? ink.edgeSign > 0 : zero >= (a.y() + b.y()) * 0.5;
    const auto color = positive ? theme::positive() : theme::negative();
    if (ink.area) {
        auto fill = color; fill.setAlpha(110);
        const double anchor = ink.edgeSign ? (positive ? ink.bottom : ink.top) : zero;
        triangle(out, a, {a.x(), anchor}, b, fill);
        triangle(out, b, {a.x(), anchor}, {b.x(), anchor}, fill);
    } else {
        // A logical-pixel ribbon stays deterministic on every QSG backend.
        triangle(out, a, a + QPointF(0, ink.stroke), b, color);
        triangle(out, b, a + QPointF(0, ink.stroke), b + QPointF(0, ink.stroke), color);
    }
}
void signedSegment(Vertex*& out, QPointF a, QPointF b, double zero, const QRectF& strip, Ink ink) {
    if (b.x() <= strip.left() || a.x() >= strip.right() || b.x() <= a.x()) return;
    const double slope = (b.y() - a.y()) / (b.x() - a.x());
    if (a.x() < strip.left()) a = {strip.left(), a.y() + (strip.left() - a.x()) * slope};
    if (b.x() > strip.right()) b = {strip.right(), b.y() + (strip.right() - b.x()) * slope};
    if ((a.y() < zero && b.y() > zero) || (a.y() > zero && b.y() < zero)) {
        const QPointF cross(a.x() + (b.x() - a.x()) * (zero - a.y()) / (b.y() - a.y()), zero);
        segment(out, a, cross, zero, ink);
        segment(out, cross, b, zero, ink);
    } else segment(out, a, b, zero, ink);
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
    if (!(capturedStyle_ == style_)) { capturedStyle_ = style_; dirty_ = true; }
    bottomInset_ = frame.context.raster.valid ? captured_.bottomInset(frame.context.time.activeTimeframeMs, frame.context.surfaceBounds.height()) : 0;
    if (mailbox_.take(current_)) { dirty_ = true; ++revision_; }
    drawing_ = captured_.stripPx > 0 && frame.context.raster.valid && frame.context.raster.dpr > 0 &&
        current_->timeframeMs == std::max<int64_t>(1000, frame.context.time.activeTimeframeMs) && current_->reset == captured_.reset &&
        current_->series.count > 0;
}
void CvdLayer::geometry(const ChartFrame& frame) {
    auto* geom = node_->geometry();
    auto* begin = geom->vertexDataAsColoredPoint2D();
    auto* out = begin;
    auto background = theme::bg0(); background.setAlphaF(capturedStyle_.fillAlpha);
    auto panel = strip_; panel.setRight(gutter_.right());
    quad(out, panel, background);
    quad(out, {panel.left(), panel.top(), panel.width(), std::min(capturedStyle_.dividerPx, panel.height())}, theme::line());
    values_ = CvdGutterValues::visible(current_->series, frame.context.raster.drawnStartMs,
                                      frame.context.raster.drawnEndMs, current_->timeframeMs);
    const int sign = edgeSign(values_, captured_.fillAnchor);
    const bool area = captured_.area && captured_.fillAnchor != CvdFillAnchor::None;
    scale_ = plotScale(current_->series.scale(frame.context.raster.drawnStartMs, frame.context.raster.drawnEndMs,
                                               current_->timeframeMs), values_, area, sign != 0);
    const CvdStripLayout layout{strip_.height(), capturedStyle_};
    const double stroke = layout.stroke();
    const Ink ink{area, area ? sign : 0, stroke,
                  strip_.top() + layout.lineY(1), strip_.top() + layout.lineY(0)};
    const auto y = [&](double value) { return strip_.top() + layout.lineY(scale_.fraction(value)); };
    const double zero = y(0);
    if (scale_.min <= 0 && scale_.max >= 0)
        quad(out, QRectF(strip_.left(), zero, strip_.width(), stroke), theme::line());
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
            signedSegment(out, {x(double(previous.timeMs) + current_->timeframeMs * 0.5), y(previous.value)}, b, zero, strip_, ink);
        } else signedSegment(out, {x(double(p.timeMs)), b.y()}, b, zero, strip_, ink);
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
    strip_ = frame.context.surfaceBounds;
    const double height = std::min(double(captured_.stripPx), strip_.height());
    strip_.setTop(strip_.bottom() - height);
    const double axisWidth = axes_ ? axes_->renderPriceAxisWidth() : AxisLayout::kPriceAxisMinWidthPx;
    gutter_ = {frame.context.itemBounds.right() + frame.context.rightGutterPx, strip_.top(), axisWidth, height};
    if (axes_) axes_->setPricePaneTop(drawing_ && !strip_.isEmpty() ? strip_.top() : -1);
    if (!gate_) return;
    gate_->setOpacity(drawing_ && !strip_.isEmpty() ? 1 : 0);
    if (!drawing_ || strip_.isEmpty()) return;
    const auto& cam = frame.context.raster;
    const Key key{cam.leftF, cam.colPxF, cam.tfMs, cam.dpr, cam.drawnStartMs, cam.drawnEndMs, strip_, gutter_, captured_.area, captured_.fillAnchor};
    if (!dirty_ && key == key_) return;
    key_ = key; dirty_ = false;
    geometry(frame);
}
void CvdLayer::addLabel(const char* text, const QRectF& box, const ChartTextAtlas& atlas) {
    if (box.height() < capturedStyle_.labelPx || box.width() <= 0) return;
    const double scale = capturedStyle_.labelPx / double(atlas.fontPx());
    double pen = 0;
    QRectF bounds;
    for (const char* c = text; *c; ++c) {
        const auto& g = atlas.glyph(QChar(*c));
        if (*c != ' ')
            bounds = bounds.united(QRectF(pen + g.bounds.left() * scale, g.bounds.top() * scale,
                                         g.bounds.width() * scale, g.bounds.height() * scale));
        pen += g.advance * scale;
    }
    // Fit the whole run before placing it; comparing local extents also avoids
    // cancellation at the box edges when its origin is fractional.
    if (bounds.width() > box.width() || bounds.height() > box.height()) return;
    pen = 0;
    for (const char* c = text; *c && glyphs_.size() < 128; ++c) {
        const auto& g = atlas.glyph(QChar(*c));
        if (*c == ' ') { pen += g.advance * scale; continue; }
        ChartGlyphInstance glyph;
        glyph.rect = QRectF(box.left() + (pen + g.bounds.left() * scale - bounds.left()),
                            box.top() + (box.height() - bounds.height()) / 2 + (g.bounds.top() * scale - bounds.top()),
                            g.bounds.width() * scale, g.bounds.height() * scale);
        glyph.uv = g.uv; glyph.color = ChartLookSettings::footprintText();
        glyphs_.push_back(glyph); pen += g.advance * scale;
    }
}
void CvdLayer::submitLabels(const ChartFrame&, LayerNodeContext& context) {
    if (!drawing_ || !context.atlasReady || context.atlas.fontPx() <= 0) return;
    if (!reserved_) reserved_ = context.text.reserve(ChartLookSettings::footprintText(), 128);
    glyphs_.clear();
    const double pad = capturedStyle_.paddingPx, label = capturedStyle_.labelRowPx;
    const CvdStripLayout layout{strip_.height(), capturedStyle_};
    if (layout.titleFits())
        addLabel(current_->label.data(), {strip_.left() + 4, strip_.top() + pad, strip_.width() - 8, label}, context.atlas);
    if (values_.valid && layout.valueRows() > 0) {
        const auto box = gutter_.adjusted(4, pad, -4, -pad);
        char value[24];
        if (layout.valueRows() >= 2) {
            CvdGutterValues::format(values_.max, value);
            addLabel(value, {box.left(), box.top(), box.width(), label}, context.atlas);
            CvdGutterValues::format(values_.min, value);
            addLabel(value, {box.left(), box.bottom() - label, box.width(), label}, context.atlas);
        }
        if (layout.valueRows() != 2) {
            CvdGutterValues::format(values_.last, value);
            const double top = strip_.top() + layout.lastTop(scale_.fraction(values_.last));
            addLabel(value, {box.left(), top, box.width(), label}, context.atlas);
        }
    }
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::High);
}
} // namespace chart
