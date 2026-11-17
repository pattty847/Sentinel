#include "chart/layers/FlowFooterLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "render/ChartTextRenderer.hpp"
#include <QSGOpacityNode>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <cstring>

namespace chart {
namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
bool validGeometry(const ChartFrame& frame) {
    const auto& cam = frame.context.raster;
    const auto& box = frame.context.surfaceBounds;
    return cam.valid && cam.dpr > 0 && cam.tfMs > 0 && cam.colPxF > 0 &&
        std::isfinite(cam.dpr + cam.tfMs + cam.colPxF + cam.leftF + box.left() + box.top() + box.width() + box.height());
}
void vertex(Vertex*& out, QPointF p, QColor c) {
    const int a = c.alpha();
    (out++)->set(float(p.x()), float(p.y()), uchar(c.red() * a / 255),
                uchar(c.green() * a / 255), uchar(c.blue() * a / 255), uchar(a));
}
void rectangle(Vertex*& out, const QRectF& r, QColor color) {
    for (const auto p : {r.topLeft(), r.bottomLeft(), r.topRight(), r.topRight(), r.bottomLeft(), r.bottomRight()})
        vertex(out, p, color);
}
// DELTA uses the direction pair, one style for every bar: the pill at 25% and the
// value in full colour. Down text is the small-text down token (#FF8C82): #F6465D on
// its own 25% pill is below 4.5:1.
constexpr double kPillAlpha = .25;
constexpr double kGutterPad = 4; // the row labels' inset and their gap to the first column
QColor deltaText(double delta) {
    return !footerPill(delta) ? ChartLookSettings::footprintText() : delta > 0 ? theme::positive() : QColor(0xFF, 0x8C, 0x82);
}
void pill(Vertex*& out, const QRectF& r, QColor color) {
    const double radius = std::min(r.height() * .5, r.width() * .5);
    const auto edge = [&](int i) {
        static constexpr double unit[] = {1, .8660254037844386, .5, 0, -.5, -.8660254037844386,
                                          -1, -.8660254037844386, -.5, 0, .5, .8660254037844386};
        const double c = unit[i % 12], s = unit[(i + 9) % 12];
        return QPointF(r.center().x() + (c >= 0 ? 1 : -1) * (r.width() * .5 - radius) + radius * c,
                       r.center().y() + radius * s);
    };
    for (int i = 0; i < 12; ++i) {
        vertex(out, r.center(), color); vertex(out, edge(i), color); vertex(out, edge(i + 1), color);
    }
}
}
FlowFooterLayer::FlowFooterLayer(const CvdLayer& cvd) : cvd_(cvd) { glyphs_.reserve(kGlyphs); }
void FlowFooterLayer::onRootRebuilt() {
    gate_ = nullptr; node_ = nullptr; reserved_ = false; vertices_ = 0; built_ = false;
}
QSGNode* FlowFooterLayer::createContainer() {
    if (!captured_.flowFooter) return nullptr;
    gate_ = new QSGOpacityNode;
    node_ = new QSGGeometryNode;
    const int capacity = kColumns * 36 + 6;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), capacity);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    std::memset(geometry->vertexData(), 0, size_t(capacity) * sizeof(Vertex));
    node_->setGeometry(geometry); node_->setFlag(QSGNode::OwnsGeometry);
    node_->setMaterial(new QSGVertexColorMaterial); node_->setFlag(QSGNode::OwnsMaterial);
    gate_->appendChildNode(node_);
    return gate_;
}
void FlowFooterLayer::sync(const ChartFrame& frame) {
    captured_ = cvd_.presentation();
    const auto& snapshot = cvd_.current();
    const auto& surface = frame.context.surfaceBounds;
    const double strip = captured_.drawable(frame.context.time.activeTimeframeMs) ? std::min(double(captured_.stripPx), surface.height()) : 0;
    layout_ = FlowFooterLayout::place(surface.top(), surface.bottom(), strip);
    bounds_ = {surface.left(), layout_.top, surface.width(), FlowFooterLayout::height};
    bottomInset_ = captured_.bottomInset(frame.context.time.activeTimeframeMs, surface.height());
    drawing_ = captured_.flowFooter && captured_.matches(frame.context.time.activeTimeframeMs) &&
        snapshot.reset == captured_.reset && snapshot.timeframeMs == captured_.dataTimeframeMs &&
        validGeometry(frame) && layout_.fits && surface.width() > 0;
}
double FlowFooterLayer::measure(const char* value, const ChartTextAtlas& atlas) const {
    double width = 0;
    for (const char* c = value; *c; ++c) width += atlas.glyph(QChar(*c)).advance;
    return width * FlowFooterLayout::textPx / atlas.fontPx();
}
// Values end at their column's right edge, so the three rows share one column grid;
// row labels start at the gutter's left edge.
void FlowFooterLayer::text(const char* value, const QRectF& box, const ChartTextAtlas& atlas, const QColor& color,
                           bool right) {
    const double scale = FlowFooterLayout::textPx / atlas.fontPx();
    double pen = right ? box.right() - measure(value, atlas) : box.left();
    for (const char* c = value; *c && glyphs_.size() < kGlyphs; ++c) {
        const auto& g = atlas.glyph(QChar(*c));
        ChartGlyphInstance glyph;
        glyph.rect = {pen + g.bounds.left() * scale, box.top() - atlas.glyphTopPx() * scale + g.bounds.top() * scale,
                      g.bounds.width() * scale, g.bounds.height() * scale};
        glyph.uv = g.uv; glyph.color = color;
        glyphs_.push_back(glyph); pen += g.advance * scale;
    }
}
void FlowFooterLayer::measureAtlas(const ChartTextAtlas& atlas) {
    const auto key = atlas.image().cacheKey();
    if (measuredAtlas_ == key && measuredFontPx_ == atlas.fontPx()) return;
    measuredAtlas_ = key; measuredFontPx_ = atlas.fontPx();
    labelWidth_ = footerLabelWidth([&](char c) {
        return atlas.glyph(QChar(c)).advance * FlowFooterLayout::textPx / atlas.fontPx();
    });
    deltaLabelWidth_ = measure("DELTA", atlas); cumLabelWidth_ = measure("CUM", atlas);
    spaceWidth_ = measure(" ", atlas);
}
void FlowFooterLayer::column(int index, double x, double width, const ChartTextAtlas& atlas, Vertex*& out) {
    const auto& snapshot = cvd_.current();
    const auto& totals = snapshot.totals[size_t(index)];
    const double values[] = {totals.delta, snapshot.series.points[size_t(index)].value, totals.volume};
    if (footerPill(totals.delta)) {
        auto color = totals.delta > 0 ? theme::positive() : theme::negative();
        color.setAlphaF(kPillAlpha);
        pill(out, {x - width * .5 - 2, layout_.rowTop(0) - 1, width + 4, FlowFooterLayout::textPx + 2}, color);
    }
    for (int row = 0; row < 3; ++row) {
        char value[24]; footerNumber(values[row], value, row < 2);
        text(value, {x - width * .5, layout_.rowTop(row), width, FlowFooterLayout::textPx}, atlas,
             row == 0 ? deltaText(totals.delta) : ChartLookSettings::footprintText(), true);
    }
    ++columns_;
}
// The gutter: DELTA / CUM / VOL, the unit once after DELTA and the CUM reset rule.
double FlowFooterLayer::gutterWidth(const ChartTextAtlas& atlas) {
    const auto& snapshot = cvd_.current();
    footerUnit(snapshot.unit.data(), unit_);
    const double delta = deltaLabelWidth_ + (unit_[0] ? spaceWidth_ + measure(unit_, atlas) : 0);
    const double cum = cumLabelWidth_ + spaceWidth_ + measure(footerResetLabel(snapshot.reset), atlas);
    return std::max(delta, cum) + 2 * kGutterPad;
}
void FlowFooterLayer::drawGutter(double width, const ChartTextAtlas& atlas) {
    if (bounds_.width() < width) return;
    const char* names[] = {"DELTA", "CUM", "VOL"};
    const QColor color = ChartLookSettings::footprintText();
    const double x = bounds_.left() + kGutterPad;
    for (int row = 0; row < 3; ++row)
        text(names[row], {x, layout_.rowTop(row), width, FlowFooterLayout::textPx}, atlas, color, false);
    if (unit_[0])
        text(unit_, {x + deltaLabelWidth_ + spaceWidth_, layout_.rowTop(0), width, FlowFooterLayout::textPx}, atlas, color, false);
    text(footerResetLabel(cvd_.current().reset), {x + cumLabelWidth_ + spaceWidth_, layout_.rowTop(1), width, FlowFooterLayout::textPx},
         atlas, color, false);
}
void FlowFooterLayer::drawColumns(const ChartFrame& frame, const ChartTextAtlas& atlas, Vertex*& out) {
    const auto& snapshot = cvd_.current();
    const auto& cam = frame.context.raster;
    measureAtlas(atlas);
    const double width = labelWidth_;
    const double labels = gutterWidth(atlas);
    const double left = bounds_.left() + labels;
    const double barWidth = cam.colPxF / cam.dpr * snapshot.timeframeMs / cam.tfMs;
    stride_ = FlowFooterLayout::stride(barWidth, width + 4);
    for (int i = 0; i < snapshot.series.count && columns_ < kColumns; ++i) {
        const auto time = snapshot.series.points[size_t(i)].timeMs;
        if (!snapshot.totals[size_t(i)].valid || !FlowFooterLayout::selected(time, snapshot.timeframeMs, stride_)) continue;
        const double x = bounds_.left() + cam.xDev(double(time) + snapshot.timeframeMs * .5) / cam.dpr;
        if (x - width * .5 - 2 < left || x + width * .5 + 2 > bounds_.right()) continue;
        column(i, x, width, atlas, out);
    }
    drawGutter(labels, atlas);
}
void FlowFooterLayer::updateNodes(const ChartFrame& frame, LayerNodeContext& context) {
    if (!gate_) return;
    const bool show = drawing_ && context.atlasReady && context.atlas.fontPx() > 0;
    gate_->setOpacity(show ? 1 : 0);
    if (!show) { glyphs_.clear(); columns_ = 0; built_ = false; return; }
    const auto& cam = frame.context.raster;
    const Key next{cvd_.snapshotRevision(), context.atlas.image().cacheKey(), bounds_,
                   cam.leftF, cam.colPxF, cam.tfMs, cam.dpr, cam.drawnStartMs, cam.drawnEndMs};
    if (built_ && key_ == next) return;
    key_ = next; built_ = true; glyphs_.clear(); columns_ = 0;
    auto* geometry = node_->geometry();
    auto* begin = geometry->vertexDataAsColoredPoint2D();
    auto* out = begin;
    rectangle(out, bounds_, ChartLookSettings::chartBackground());
    drawColumns(frame, context.atlas, out);
    const int used = int(out - begin);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(used);
#else
    if (used < vertices_) std::memset(out, 0, size_t(vertices_ - used) * sizeof(Vertex));
#endif
    vertices_ = used; geometry->markVertexDataDirty(); node_->markDirty(QSGNode::DirtyGeometry);
}
void FlowFooterLayer::submitLabels(const ChartFrame&, LayerNodeContext& context) {
    if (!drawing_ || glyphs_.empty() || !context.atlasReady) return;
    if (!reserved_)
        reserved_ = context.text.reserve(ChartLookSettings::footprintText(), kGlyphs) &&
            context.text.reserve(deltaText(1), kColumns * 6) && context.text.reserve(deltaText(-1), kColumns * 6);
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::High);
}
} // namespace chart
