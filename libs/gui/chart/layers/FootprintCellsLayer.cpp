#include "chart/layers/FootprintCellsLayer.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/core/FlowInk.hpp"
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
// Left half is sell aggression (bidVol), right half is buy aggression (askVol).
QColor sideColour(bool sell) {
    return sell ? ChartLookSettings::footprintSellAggr() : ChartLookSettings::footprintBuyAggr();
}
QColor inkColour(bool light, FlowTier tier) {
    auto colour = light ? ChartLookSettings::footprintText() : ChartLookSettings::footprintInkDark();
    colour.setAlpha(int(std::lround(flowTextAlpha(tier) * 255)));
    return colour;
}
FlowRgb rgb(const QColor& c) { return {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue())}; }
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
        data.cellCount > 0 && data.tick == cam.tick && data.timeframeMs == frame.context.time.activeTimeframeMs;
}
QString FootprintCellsLayer::emptyStateText(const ChartFrame& frame) const {
    const auto& cam = frame.context.raster;
    const auto& data = *current_;
    if (!frame.context.overlays.footprint || stage_ == FootprintStage::Hidden || !data.valid ||
        data.tick != cam.tick || data.timeframeMs != frame.context.time.activeTimeframeMs)
        return {};
    if (data.windowFromMs != cam.drawnStartMs || data.windowToMs != cam.drawnEndMs ||
        data.windowLow != cam.drawnMinPrice || data.windowHigh != cam.drawnMaxPrice ||
        data.maxRowVolume > 0) return {};
    if (data.windowState == FlowSnapshot::WindowState::Unavailable)
        return QStringLiteral("Footprint: trade data unavailable for this window");
    if (data.windowState == FlowSnapshot::WindowState::Ready)
        return QStringLiteral("Footprint: no trade data recorded for this window");
    return {};
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
QRectF FootprintCellsLayer::cardRect(const ChartFrame& frame, const FlowColumn& bar) const {
    return cellRect(frame, bar, {bar.lowRow, bar.highRow - bar.lowRow, 0, 0});
}
void FootprintCellsLayer::rowGeometry(QSGGeometry::ColoredPoint2D* vertices, int& used,
    const ChartFrame& frame, const FlowColumn& bar, const FlowCell& cell) {
    const auto rect = cellRect(frame, bar, cell);
    if (rect.isEmpty()) return;
    const auto& cam = frame.context.raster;
    const double left = frame.context.surfaceBounds.left() + cam.xDev(double(bar.startMs)) / cam.dpr;
    const double width = cam.colPxF / cam.dpr;
    const double mid = left + (width + kFootprintCandleSpace) * .5;
    const double half = std::max(0.0, (width - kFootprintCandleSpace) * .5 - 1);
    const double price = (double(cell.row) + cell.span * .5) * cam.tick;
    for (bool sell : {true, false}) {
        const double volume = double(sell ? cell.bidVol : cell.askVol) / bar.baseScale;
        if (flowTier(volume, price, double(bar.maxSideVol) / bar.baseScale, presentation_.dustNotional) == FlowTier::Empty) continue;
        const QRectF fill(sell ? mid - .5 - half : mid + .5, rect.top(), half, rect.height());
        rectangle(vertices + used, fill.intersected(frame.context.surfaceBounds), sideColour(sell),
                  flowFillAlpha(volume, normaliser(bar)));
        used += 6;
    }
}
double FootprintCellsLayer::normaliser(const FlowColumn& bar) const {
    return presentation_.windowIntensity ? current_->maxRowVolume : double(bar.maxSideVol) / bar.baseScale;
}
// One band-wide cell: total volume intensity, tinted by the side that dominates.
void FootprintCellsLayer::stripGeometry(QSGGeometry::ColoredPoint2D* vertices, int& used,
    const ChartFrame& frame, const FlowColumn& bar, const FlowCell& cell) {
    const auto rect = cellRect(frame, bar, cell);
    if (rect.isEmpty()) return;
    const double volume = double(cell.bidVol + cell.askVol) / bar.baseScale;
    const double maximum = double(bar.maxRowVol) / bar.baseScale;
    const double price = (double(cell.row) + cell.span * .5) * frame.context.raster.tick;
    const auto tier = flowTier(volume, price, maximum, presentation_.dustNotional);
    if (tier == FlowTier::Empty || tier == FlowTier::Dust) return;
    const double normaliser = presentation_.windowIntensity ? current_->maxRowVolume : maximum;
    rectangle(vertices + used, rect, sideColour(cell.askVol < cell.bidVol), volume / normaliser);
    used += 6;
}
void FootprintCellsLayer::cells(const ChartFrame& frame) {
    auto* geometry = node_->geometry();
    const int needed = current_->cellCount * 12 + current_->barCount * 12;
    if (needed > capacity_) {
        capacity_ = candle_pixels::capacityFor(capacity_, needed);
        geometry->allocate(capacity_);
        std::memset(geometry->vertexData(), 0, size_t(capacity_) * sizeof(QSGGeometry::ColoredPoint2D));
    }
    auto* vertices = geometry->vertexDataAsColoredPoint2D();
    int used = 0;
    cardCount_ = 0;
    for (int b = 0; b < current_->barCount; ++b) {
        const auto& bar = current_->bars[size_t(b)];
        if (stage_ == FootprintStage::Strip) {
            for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i)
                stripGeometry(vertices, used, frame, bar, current_->cells[size_t(i)]);
            continue;
        }
        const auto card = cardRect(frame, bar);
        if (!bar.cellCount || card.isEmpty()) continue;
        cards_[cardCount_++] = card;
        rectangle(vertices + used, card, ChartLookSettings::chartBackground(), 1); used += 6;
        const auto& cam = frame.context.raster;
        const double left = frame.context.surfaceBounds.left() + cam.xDev(double(bar.startMs)) / cam.dpr;
        const double mid = left + (cam.colPxF / cam.dpr + kFootprintCandleSpace) * .5;
        rectangle(vertices + used, QRectF(mid - .5, card.top(), 1, card.height()).intersected(card),
                  ChartLookSettings::footprintText(), .22); used += 6;
        for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i)
            rowGeometry(vertices, used, frame, bar, current_->cells[size_t(i)]);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(used);
#else
    if (used < vertices_) std::memset(vertices + used, 0, size_t(vertices_ - used) * sizeof(*vertices));
#endif
    vertices_ = used;
    geometry->markVertexDataDirty(); node_->markDirty(QSGNode::DirtyGeometry);
}
double FootprintCellsLayer::windowMaximum(const ChartFrame& frame) const {
    double maximum = 0;
    for (int b = 0; b < current_->barCount; ++b) {
        const auto& bar = current_->bars[size_t(b)];
        for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i) {
            const auto& cell = current_->cells[size_t(i)];
            const auto volume = stage_ == FootprintStage::Strip ? cell.bidVol + cell.askVol : std::max(cell.bidVol, cell.askVol);
            if (!cellRect(frame, bar, cell).isEmpty()) maximum = std::max(maximum, double(volume) / bar.baseScale);
        }
    }
    return maximum;
}
void FootprintCellsLayer::updateNodes(const ChartFrame& frame, LayerNodeContext&) {
    if (!gate_) return;
    gate_->setOpacity(drawing_ ? 1 : 0);
    if (!drawing_) return;
    const auto& cam = frame.context.raster;
    const Key key{cam.topF, cam.leftF, cam.rowPxF, cam.colPxF, cam.tick, cam.dpr,
                  frame.context.surfaceBounds, presentation_.windowIntensity, presentation_.dustNotional, stage_};
    if (built_ && key == key_) return;
    key_ = key; built_ = true; labelsDirty_ = true;
    if (presentation_.windowIntensity) current_->maxRowVolume = windowMaximum(frame);
    cells(frame);
}
void FootprintCellsLayer::number(const Number& value, const ChartTextAtlas& atlas) {
    const auto& box = value.box;
    const auto tier = flowTier(value.volume, value.price, value.maximum, presentation_.dustNotional);
    if (tier == FlowTier::Empty) return;
    const bool light = flowInkIsLight(rgb(sideColour(value.bid)), value.fillAlpha, rgb(ChartLookSettings::chartBackground()),
                                      rgb(ChartLookSettings::footprintText()), rgb(ChartLookSettings::footprintInkDark()));
    const auto colour = inkColour(light, tier);
    char text[24];
    const int length = std::min(23, flowNumber(value.volume, text));
    const double scale = kFootprintTextPx / atlas.fontPx();
    double width = 0, top = 0, bottom = 0;
    for (int i = 0; i < length; ++i) {
        const auto& glyph = atlas.glyph(QChar(text[i]));
        width += glyph.advance * scale;
        top = std::min(top, glyph.bounds.top() * scale);
        bottom = std::max(bottom, glyph.bounds.bottom() * scale);
    }
    const double height = bottom - top;
    if (width > box.width() || height > box.height() || glyphs_.size() + size_t(length) > kMaxGlyphs) return;
    double pen = value.bid ? box.right() - width : box.left();
    const double baseline = box.center().y() - height * 0.5 - top;
    for (int i = 0; i < length; ++i) {
        const auto& g = atlas.glyph(QChar(text[i]));
        ChartGlyphInstance glyph;
        glyph.rect = QRectF(pen + g.bounds.left() * scale, baseline + g.bounds.top() * scale,
                            g.bounds.width() * scale, g.bounds.height() * scale);
        glyph.uv = g.uv; glyph.color = colour;
        glyphs_.push_back(glyph);
        pen += g.advance * scale;
    }
}
void FootprintCellsLayer::labels(const ChartFrame& frame, const ChartTextAtlas& atlas, double bottomInset) {
    auto textBounds = frame.context.surfaceBounds;
    textBounds.setBottom(textBounds.bottom() - bottomInset);
    for (int b = 0; b < current_->barCount; ++b) {
        const auto& bar = current_->bars[size_t(b)];
        for (int i = bar.firstCell; i < bar.firstCell + bar.cellCount; ++i) {
            const auto& cell = current_->cells[size_t(i)];
            const auto rect = cellRect(frame, bar, cell);
            if (rect.isEmpty()) continue;
            const auto& cam = frame.context.raster;
            const double left = frame.context.surfaceBounds.left() + cam.xDev(double(bar.startMs)) / cam.dpr;
            const double right = left + cam.colPxF / cam.dpr;
            const double mid = (left + right + kFootprintCandleSpace) * .5;
            const double price = (double(cell.row) + cell.span * .5) * cam.tick;
            const double maximum = double(bar.maxSideVol) / bar.baseScale;
            const QRectF bid(left + kFootprintCandleSpace + 1, rect.top(), mid - left - kFootprintCandleSpace - 3, rect.height());
            const QRectF ask(mid + 2, rect.top(), right - mid - 3, rect.height());
            const double sell = double(cell.bidVol) / bar.baseScale, buy = double(cell.askVol) / bar.baseScale;
            if (textBounds.contains(bid)) number({sell, price, maximum, flowFillAlpha(sell, normaliser(bar)), bid, true}, atlas);
            if (textBounds.contains(ask)) number({buy, price, maximum, flowFillAlpha(buy, normaliser(bar)), ask, false}, atlas);
        }
    }
}
void FootprintCellsLayer::submitLabels(const ChartFrame& frame, LayerNodeContext& context, double bottomInset) {
    if (!drawing_ || !context.atlasReady) {
        glyphs_.clear(); labelsDirty_ = true;
        return;
    }
    if (!reserved_) {
        reserved_ = true;
        for (auto tier : {FlowTier::Dust, FlowTier::Normal, FlowTier::Heavy}) {
            for (bool light : {true, false}) reserved_ = context.text.reserve(inkColour(light, tier), kMaxGlyphs) && reserved_;
        }
    }
    if (stage_ != FootprintStage::Numbers || context.atlas.fontPx() <= 0) {
        glyphs_.clear(); labelsDirty_ = true;
        return;
    }
    const auto atlasKey = context.atlas.image().cacheKey();
    if (labelsDirty_ || atlasKey_ != atlasKey || labelBottomInset_ != bottomInset) {
        glyphs_.clear(); labels(frame, context.atlas, bottomInset);
        labelBottomInset_ = bottomInset;
        atlasKey_ = atlasKey; labelsDirty_ = false;
    }
    context.text.submitGlyphs(glyphs_, ChartTextRenderer::Priority::Low);
}
} // namespace chart
