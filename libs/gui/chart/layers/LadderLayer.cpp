#include "chart/layers/LadderLayer.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/ChartTextRenderer.hpp"
#include "SentinelLogging.hpp"
#include <QElapsedTimer>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGVertexColorMaterial>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace chart {
namespace {
constexpr int kVerticesPerBar = 6;
constexpr int kMaxVertices = LadderSnapshot::kMaxRows * 2 * kVerticesPerBar;
struct Rgba { uchar r, g, b, a; };
// Premultiplied (QSGVertexColorMaterial): plain bid and ask colours.
constexpr Rgba kBid{34, 150, 139, 230};
constexpr Rgba kAsk{215, 75, 72, 230};
// Wall radar bars (the owner tunes these straight colours and alphas live): the Electric
// palette's top stops and the warn colour for a recent flicker, stored premultiplied.
constexpr Rgba premultiply(Rgba c) {
    return {uchar((c.r * c.a + 127) / 255), uchar((c.g * c.a + 127) / 255), uchar((c.b * c.a + 127) / 255), c.a};
}
constexpr Rgba kBidWallSrgb{0, 255, 255, 220};
constexpr Rgba kAskWallSrgb{255, 200, 0, 220};
constexpr Rgba kHotWallSrgb{255, 208, 77, 230};
constexpr Rgba kBidWall = premultiply(kBidWallSrgb);
constexpr Rgba kAskWall = premultiply(kAskWallSrgb);
constexpr Rgba kHotWall = premultiply(kHotWallSrgb);

Rgba barColour(const LadderSnapshot& s, uint8_t flags, uint8_t large, uint8_t hot, Rgba plain, Rgba wall) {
    if (!s.radar || !(flags & large)) return plain;
    return (flags & hot) ? kHotWall : wall;
}

// A vertex-colour triangle node of a fixed capacity, drawing nothing yet.
QSGGeometryNode* colouredNode(int capacity) {
    auto* node = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), capacity);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(0);
#else
    std::memset(geometry->vertexData(), 0, size_t(capacity) * sizeof(QSGGeometry::ColoredPoint2D));
#endif
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial());
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

// Draws `used` vertices: degenerate (zeroed) after them on Qt before 6.10.
void commitVertices(QSGGeometryNode* node, int used, int& previous) {
    auto* geometry = node->geometry();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(used);
#else
    auto* v = geometry->vertexDataAsColoredPoint2D();
    if (used < previous) std::memset(v + used, 0, size_t(previous - used) * sizeof(*v));
#endif
    previous = used;
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
}

void writeBar(QSGGeometry::ColoredPoint2D* v, float x0, float x1, float y0, float y1, Rgba c) {
    const float xs[kVerticesPerBar] = {x0, x1, x0, x0, x1, x1};
    const float ys[kVerticesPerBar] = {y0, y0, y1, y1, y0, y1};
    for (int i = 0; i < kVerticesPerBar; ++i) v[i].set(xs[i], ys[i], c.r, c.g, c.b, c.a);
}
} // namespace

LadderMailbox::LadderMailbox()
    : back_(std::make_unique<LadderSnapshot>()), pending_(std::make_unique<LadderSnapshot>()) {}
LadderMailbox::~LadderMailbox() = default;

void LadderMailbox::post() {
    std::lock_guard<std::mutex> lock(mutex_);
    back_.swap(pending_);
    fresh_ = true;
}

bool LadderMailbox::take(std::unique_ptr<LadderSnapshot>& current, uint64_t minGeneration) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fresh_) return false;
    fresh_ = false;
    if (pending_->generation < minGeneration) return false;
    current.swap(pending_);
    return true;
}

LadderLayer::LadderLayer() : current_(std::make_unique<LadderSnapshot>()) {}
LadderLayer::~LadderLayer() = default;

void LadderLayer::onRootRebuilt() {
    gate_ = nullptr;
    clip_ = nullptr;
    bars_ = nullptr;
    badges_ = nullptr;
    built_ = false;
    reserved_ = false; // the text renderer drops its nodes with the root
}

QSGNode* LadderLayer::createContainer() {
    gate_ = new QSGOpacityNode();
    gate_->setOpacity(0.0);
    clip_ = new QSGClipNode();
    auto* clipGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
    clipGeometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    clip_->setGeometry(clipGeometry);
    clip_->setFlag(QSGNode::OwnsGeometry);
    clip_->setIsRectangular(true);
    bars_ = colouredNode(kMaxVertices);
    clip_->appendChildNode(bars_);
    gate_->appendChildNode(clip_);
    badges_ = colouredNode(kMaxBadgeVertices); // over the chart: not under the strip's clip
    gate_->appendChildNode(badges_);
    vertices_ = 0;
    badgeVertices_ = 0;
    ++creations_;
    return gate_;
}

bool LadderLayer::drawable(const ChartFrame& frame) const {
    const auto& cam = frame.context.raster;
    const auto& s = *current_;
    if (frame.context.rightGutterPx <= 0 || !cam.valid || !(cam.tick > 0) || !s.valid || !s.complete) return false;
    if (s.rowCount <= 0 || std::abs(s.binPrice() - cam.tick) > 1e-9 * cam.tick) return false;
    const auto window = ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, s.binPrice());
    return window.lo >= s.binLo && window.hi < s.binLo + s.rowCount;
}

void LadderLayer::sync(const ChartFrame& frame) {
    if (mailbox_.take(current_, generation_)) generation_ = current_->generation;
    drawing_ = drawable(frame);
    const auto& cam = frame.context.raster;
    if (drawing_ || frame.context.rightGutterPx <= 0 || !cam.valid || !(cam.tick > 0)) return;
    // Built for another frame (tick, rows or drag): ask the feed once per snapshot.
    const auto window = ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, cam.tick);
    const auto& s = *current_;
    const bool sameFrame = s.evalTick == cam.tick && s.evalLo == window.lo && s.evalHi == window.hi;
    if (!sameFrame && requestedFor_ != s.serial) {
        requestedFor_ = s.serial;
        wantsRepublish_.store(true);
    }
}

void LadderLayer::updateNodes(const ChartFrame& frame, LayerNodeContext& context) {
    if (!gate_) return;
    const double opacity = drawing_ ? 1.0 : 0.0;
    if (gate_->opacity() != opacity) gate_->setOpacity(opacity);
    if (!drawing_) return;
    const auto& f = frame.context;
    const Key key{current_->serial, f.raster.topF, f.raster.rowPxF, f.raster.tick, f.raster.dpr,
                  f.itemBounds.x() + f.itemBounds.width(), f.rightGutterPx, f.surfaceBounds.top(),
                  f.surfaceBounds.height(), context.atlasReady ? &context.atlas : nullptr};
    if (built_ && key == key_) return;
    key_ = key;
    built_ = true;
    rebuild(frame);
}

void LadderLayer::rebuild(const ChartFrame& frame) {
    const auto& f = frame.context;
    const auto& cam = f.raster;
    const auto& s = *current_;
    const QRectF strip(key_.left, f.surfaceBounds.top(), f.rightGutterPx, f.surfaceBounds.height());
    if (clip_->clipRect() != strip) {
        clip_->setClipRect(strip);
        QSGGeometry::updateRectGeometry(clip_->geometry(), strip);
        clip_->markDirty(QSGNode::DirtyGeometry);
    }
    const double bin = s.binPrice();
    const auto window = ladderWindow(cam.drawnMinPrice, cam.drawnMaxPrice, bin);
    const int first = int(window.lo - s.binLo), last = int(window.hi - s.binLo);
    float peak = 0;
    for (int i = first; i <= last; ++i) peak = std::max({peak, s.rows[size_t(i)].bid, s.rows[size_t(i)].ask});
    const float x0 = float(strip.left()) + kInsetPx, x1 = float(strip.right()) - kInsetPx;
    const float mid = 0.5f * (x0 + x1), half = std::max(0.0f, 0.5f * (x1 - x0));
    auto* v = bars_->geometry()->vertexDataAsColoredPoint2D();
    int count = 0;
    for (int i = first; peak > 0 && i <= last; ++i) {
        const auto& row = s.rows[size_t(i)];
        const double k = double(s.binLo + i);
        const float yTop = float(f.surfaceBounds.top() + cam.yDev((k + 1) * bin) / cam.dpr);
        const float yBottom = float(f.surfaceBounds.top() + cam.yDev(k * bin) / cam.dpr);
        if (row.bid > 0)
            writeBar(v + (count++) * kVerticesPerBar, mid - half * row.bid / peak, mid, yTop, yBottom,
                     barColour(s, row.flags, LadderRow::kBidLarge, LadderRow::kBidHot, kBid, kBidWall));
        if (row.ask > 0)
            writeBar(v + (count++) * kVerticesPerBar, mid, mid + half * row.ask / peak, yTop, yBottom,
                     barColour(s, row.flags, LadderRow::kAskLarge, LadderRow::kAskHot, kAsk, kAskWall));
    }
    commitVertices(bars_, count * kVerticesPerBar, vertices_);
    ++dirtyMarks_;
    rebuildBadges(frame);
    ++rebuilds_;
}

void LadderLayer::rebuildBadges(const ChartFrame& frame) {
    QElapsedTimer timer;
    timer.start();
    badgeLayout_.layout(*current_, frame.context, key_.atlas);
    commitVertices(badges_, badgeLayout_.writeGeometry(badges_->geometry()->vertexDataAsColoredPoint2D()),
                   badgeVertices_);
    ++dirtyMarks_;
    if (!current_->radar || !key_.atlas) return; // no layout ran
    const auto& st = badgeLayout_.stats();
    sLog_Probe("ladder.badges", "serial=" << current_->serial << " rows=" << st.rowBadges << " dropped=" << st.dropped
               << " pills=" << st.edgePills << " glyphs=" << st.glyphs << " vertices=" << badgeVertices_
               << " ns=" << timer.nsecsElapsed());
}

bool LadderLayer::reserveText(ChartTextRenderer& text) const {
    return text.reserve(QColor::fromRgba(kBidText), kMaxBadgeGlyphs) &&
           text.reserve(QColor::fromRgba(kAskText), kMaxBadgeGlyphs) &&
           text.reserve(QColor::fromRgba(kHotText), kMaxBadgeGlyphs);
}

void LadderLayer::submitLabels(const ChartFrame&, LayerNodeContext& context) {
    if (!gate_ || !drawing_ || !current_->radar || !context.atlasReady) return;
    if (!reserved_) reserved_ = reserveText(context.text);
    if (!badgeLayout_.glyphs().empty())
        context.text.submitGlyphs(badgeLayout_.glyphs(), ChartTextRenderer::Priority::Low);
}

void LadderLayer::afterRendering() {
    if (wantsRepublish_.exchange(false) && republish_) republish_();
}
} // namespace chart
