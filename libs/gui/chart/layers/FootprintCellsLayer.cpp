#include "chart/layers/FootprintCellsLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/shell/ChartLookSettings.hpp"
#include "render/CandlePixelGeometry.hpp"
#include "render/ChartTextRenderer.hpp"
#include <QSGOpacityNode>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cstring>

namespace chart {
namespace {
void rectangle(QSGGeometry::ColoredPoint2D* vertices, const QRectF& rect, const QColor& colour, double intensity) {
    const auto alpha = uchar(std::clamp(intensity, 0.0, 1.0) * colour.alpha());
    const uchar red = uchar(colour.red() * alpha / 255), green = uchar(colour.green() * alpha / 255);
    const uchar blue = uchar(colour.blue() * alpha / 255);
    const QPointF points[] = {rect.topLeft(), rect.bottomLeft(), rect.topRight(),
                             rect.topRight(), rect.bottomLeft(), rect.bottomRight()};
    for (int i = 0; i != 6; ++i) vertices[i].set(float(points[i].x()), float(points[i].y()), red, green, blue, alpha);
}
}
FootprintCellsLayer::FootprintCellsLayer() : current_(std::make_unique<FlowSnapshot>()) {
    glyphs_.reserve(kMaxGlyphs);
}
void FootprintCellsLayer::onRootRebuilt() {
    gate_ = nullptr; node_ = nullptr; capacity_ = 0; vertices_ = 0; reserved_ = false;
    built_ = false; labelsDirty_ = true;
}
QSGNode* FootprintCellsLayer::createContainer() {
    gate_ = new QSGOpacityNode;
    gate_->setOpacity(0);
    node_ = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    node_->setGeometry(geometry); node_->setFlag(QSGNode::OwnsGeometry);
    node_->setMaterial(new QSGVertexColorMaterial); node_->setFlag(QSGNode::OwnsMaterial);
    gate_->appendChildNode(node_);
    return gate_;
}
void FootprintCellsLayer::sync(const ChartFrame& frame) {
    if (mailbox_.take(current_)) built_ = false;
    stage_ = presentation_.stage(frame.context.raster);
    const auto& cam = frame.context.raster;
    const auto& data = *current_;
    drawing_ = frame.context.overlays.footprint && stage_ != FootprintStage::Hidden && data.valid &&
        data.cellCount > 0 && data.tick == cam.tick && data.timeframeMs == frame.context.time.activeTimeframeMs &&
        cam.drawnStartMs >= data.fromMs && cam.drawnEndMs <= data.toMs &&
        cam.drawnMinPrice >= data.low && cam.drawnMaxPrice <= data.high;
}
double FootprintCellsLayer::intensity(const FlowColumn& bar, const FlowCell& cell,
                                      const FlowSnapshot& snapshot, bool window) {
    const double volume = double(cell.bidVol + cell.askVol);
    const double denominator = window ? snapshot.maxRowVolume * double(bar.baseScale) : double(bar.maxRowVol);
    return denominator > 0 ? std::clamp(volume / denominator, 0.0, 1.0) : 0;
}
QRectF FootprintCellsLayer::cellRect(const ChartFrame& frame, const FlowColumn& bar, const FlowCell& cell) const {
    const auto& f = frame.context;
    const auto& cam = f.raster;
    const double left = f.surfaceBounds.left() + cam.xDev(double(bar.startMs)) / cam.dpr;
    const double right = f.surfaceBounds.left() + cam.xDev(double(bar.startMs) + current_->timeframeMs) / cam.dpr;
    const double top = f.surfaceBounds.top() + cam.yDev((double(cell.row) + cell.span) * cam.tick) / cam.dpr;
    const double bottom = f.surfaceBounds.top() + cam.yDev(double(cell.row) * cam.tick) / cam.dpr;
    return QRectF(QPointF(left, top), QPointF(right, bottom)).intersected(f.surfaceBounds);
}
void FootprintCellsLayer::cells(const ChartFrame& frame) {
    auto* geometry = node_->geometry();
    const int needed = current_->cellCount * 12;
    if (needed > capacity_) {
        capacity_ = candle_pixels::capacityFor(capacity_, needed);
        geometry->allocate(capacity_);
        std::memset(geometry->vertexData(), 0, size_t(capacity_) * sizeof(QSGGeometry::ColoredPoint2D));
    }
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int used = 0;
    for (int b = 0; b < current_->barCount; ++b) {
        const auto& bar = current_->bars[size_t(b)];
        for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i) {
            const auto& cell = current_->cells[size_t(i)];
            const auto rect = cellRect(frame, bar, cell);
            const double mid = frame.context.surfaceBounds.left() + frame.context.raster.xDev(
                double(bar.startMs) + current_->timeframeMs * 0.5) / frame.context.raster.dpr;
            const double fill = intensity(bar, cell, *current_, presentation_.windowIntensity);
            if (cell.bidVol > 0) {
                const QRectF half(rect.left(), rect.top(), std::max(0.0, std::min(mid, rect.right()) - rect.left()), rect.height());
                rectangle(vertices + used, half, ChartLookSettings::footprintBid(), fill); used += 6;
            }
            if (cell.askVol > 0) {
                const double left = std::max(rect.left(), mid);
                rectangle(vertices + used, QRectF(left, rect.top(), std::max(0.0, rect.right() - left), rect.height()),
                          ChartLookSettings::footprintAsk(), fill); used += 6;
            }
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(used);
#else
    if (used < vertices_) std::memset(vertices + used, 0, size_t(vertices_ - used) * sizeof(*vertices));
#endif
    vertices_ = used;
    geometry->markVertexDataDirty(); node_->markDirty(QSGNode::DirtyGeometry);
}
void FootprintCellsLayer::updateNodes(const ChartFrame& frame, LayerNodeContext&) {
    if (!gate_) return;
    gate_->setOpacity(drawing_ ? 1 : 0);
    if (!drawing_) return;
    const auto& cam = frame.context.raster;
    const Key key{cam.topF, cam.leftF, cam.rowPxF, cam.colPxF, cam.tick, cam.dpr,
                  frame.context.surfaceBounds, presentation_.windowIntensity};
    if (built_ && key == key_) return;
    key_ = key; built_ = true; labelsDirty_ = true;
    cells(frame);
}
void FootprintCellsLayer::number(double value, const QRectF& box, const ChartTextAtlas& atlas) {
    char text[24];
    const int length = std::min(23, footprintNumber(value, text));
    const double scale = ChartLookSettings::kFootprintTextPx / atlas.fontPx();
    double width = 0;
    for (int i = 0; i < length; ++i) width += atlas.glyph(QChar(text[i])).advance * scale;
    const double height = (atlas.glyphBottomPx() - atlas.glyphTopPx()) * scale;
    if (width > box.width() || height > box.height() || glyphs_.size() + size_t(length) > kMaxGlyphs) return;
    double pen = box.center().x() - width * 0.5;
    const double baseline = box.center().y() - height * 0.5 - atlas.glyphTopPx() * scale;
    for (int i = 0; i < length; ++i) {
        const auto& g = atlas.glyph(QChar(text[i]));
        ChartGlyphInstance glyph;
        glyph.rect = QRectF(pen + g.bounds.left() * scale, baseline + g.bounds.top() * scale,
                            g.bounds.width() * scale, g.bounds.height() * scale);
        glyph.uv = g.uv; glyph.color = ChartLookSettings::footprintText();
        glyphs_.push_back(glyph);
        pen += g.advance * scale;
    }
}
void FootprintCellsLayer::labels(const ChartFrame& frame, const ChartTextAtlas& atlas) {
    for (int b = 0; b < current_->barCount; ++b) {
        const auto& bar = current_->bars[size_t(b)];
        for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i) {
            const auto& cell = current_->cells[size_t(i)];
            const auto rect = cellRect(frame, bar, cell);
            const double mid = frame.context.surfaceBounds.left() + frame.context.raster.xDev(
                double(bar.startMs) + current_->timeframeMs * 0.5) / frame.context.raster.dpr;
            number(double(cell.bidVol) / bar.baseScale, QRectF(rect.left(), rect.top(), mid - rect.left(), rect.height()), atlas);
            number(double(cell.askVol) / bar.baseScale, QRectF(mid, rect.top(), rect.right() - mid, rect.height()), atlas);
        }
    }
}
void FootprintCellsLayer::submitLabels(const ChartFrame& frame, LayerNodeContext& context) {
    if (!drawing_ || !context.atlasReady) {
        glyphs_.clear(); labelsDirty_ = true;
        return;
    }
    if (!reserved_) reserved_ = context.text.reserve(ChartLookSettings::footprintText(), kMaxGlyphs);
    if (stage_ != FootprintStage::Numbers || context.atlas.fontPx() <= 0) {
        glyphs_.clear(); labelsDirty_ = true;
        return;
    }
    const auto atlasKey = context.atlas.image().cacheKey();
    if (labelsDirty_ || atlasKey_ != atlasKey) {
        glyphs_.clear(); labels(frame, context.atlas);
        atlasKey_ = atlasKey; labelsDirty_ = false;
    }
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::Low);
}
} // namespace chart
