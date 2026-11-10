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
void FlowFooterLayer::text(const char* value, const QRectF& box, const ChartTextAtlas& atlas) {
    const double scale = FlowFooterLayout::textPx / atlas.fontPx();
    double pen = box.center().x() - measure(value, atlas) * .5;
    for (const char* c = value; *c && glyphs_.size() < kGlyphs; ++c) {
        const auto& g = atlas.glyph(QChar(*c));
        ChartGlyphInstance glyph;
        glyph.rect = {pen + g.bounds.left() * scale, box.top() - atlas.glyphTopPx() * scale + g.bounds.top() * scale,
                      g.bounds.width() * scale, g.bounds.height() * scale};
        glyph.uv = g.uv; glyph.color = ChartLookSettings::footprintText();
        glyphs_.push_back(glyph); pen += g.advance * scale;
    }
}
double FlowFooterLayer::widest(const ChartTextAtlas& atlas) const {
    double width = 0;
    const auto& snapshot = cvd_.current();
    for (int i = 0; i < snapshot.series.count; ++i) {
        const auto& totals = snapshot.totals[size_t(i)];
        if (!totals.valid) continue;
        const double values[] = {totals.delta, snapshot.series.points[size_t(i)].value, totals.volume};
        for (int row = 0; row < 3; ++row) {
            char value[24]; footerNumber(values[row], value, row < 2);
            width = std::max(width, measure(value, atlas));
        }
    }
    return width;
}
void FlowFooterLayer::column(int index, double x, double width, const ChartTextAtlas& atlas, Vertex*& out) {
    const auto& snapshot = cvd_.current();
    const auto& totals = snapshot.totals[size_t(index)];
    const double values[] = {totals.delta, snapshot.series.points[size_t(index)].value, totals.volume};
    if (totals.delta != 0 && maximum_ > 0) {
        auto color = totals.delta > 0 ? ChartLookSettings::footprintBid() : ChartLookSettings::footprintAsk();
        color.setAlphaF(.15 + .65 * std::clamp(std::abs(totals.delta) / maximum_, 0.0, 1.0));
        pill(out, {x - width * .5 - 2, layout_.rowTop(0) - 1, width + 4, FlowFooterLayout::textPx + 2}, color);
    }
    for (int row = 0; row < 3; ++row) {
        char value[24]; footerNumber(values[row], value, row < 2);
        text(value, {x - width * .5, layout_.rowTop(row), width, FlowFooterLayout::textPx}, atlas);
    }
    ++columns_;
}
void FlowFooterLayer::drawColumns(const ChartFrame& frame, const ChartTextAtlas& atlas, Vertex*& out) {
    const auto& snapshot = cvd_.current();
    const auto& cam = frame.context.raster;
    const double width = widest(atlas);
    const double labels = measure("DELTA", atlas) + 8;
    const double left = bounds_.left() + labels;
    const double barWidth = cam.colPxF / cam.dpr * snapshot.timeframeMs / cam.tfMs;
    const auto k = FlowFooterLayout::stride(barWidth, std::max(width + 4, bounds_.width() / kColumns));
    maximum_ = 0;
    for (int i = 0; i < snapshot.series.count; ++i) {
        const auto time = snapshot.series.points[size_t(i)].timeMs;
        if (double(time) < cam.drawnEndMs && double(time) + snapshot.timeframeMs > cam.drawnStartMs && snapshot.totals[size_t(i)].valid)
            maximum_ = std::max(maximum_, std::abs(snapshot.totals[size_t(i)].delta));
    }
    for (int i = 0; i < snapshot.series.count && columns_ < kColumns; ++i) {
        const auto time = snapshot.series.points[size_t(i)].timeMs;
        if (!snapshot.totals[size_t(i)].valid || !FlowFooterLayout::selected(time, snapshot.timeframeMs, k)) continue;
        const double x = bounds_.left() + cam.xDev(double(time) + snapshot.timeframeMs * .5) / cam.dpr;
        if (x - width * .5 - 2 < left || x + width * .5 + 2 > bounds_.right()) continue;
        column(i, x, width, atlas, out);
    }
    if (bounds_.width() < labels) return;
    const char* names[] = {"DELTA", "CUM", "VOL"};
    for (int row = 0; row < 3; ++row)
        text(names[row], {bounds_.left() + 2, layout_.rowTop(row), labels - 4, FlowFooterLayout::textPx}, atlas);
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
    if (!reserved_) reserved_ = context.text.reserve(ChartLookSettings::footprintText(), kGlyphs);
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::High);
}
} // namespace chart
