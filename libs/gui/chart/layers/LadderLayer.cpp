#include "chart/layers/LadderLayer.hpp"
#include "chart/layers/LadderGrid.hpp"
#include "chart/core/ChartFrame.hpp"
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
    built_ = false;
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
    bars_ = new QSGGeometryNode();
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), kMaxVertices);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(0);
#else
    std::memset(geometry->vertexData(), 0, size_t(kMaxVertices) * sizeof(QSGGeometry::ColoredPoint2D));
#endif
    bars_->setGeometry(geometry);
    bars_->setFlag(QSGNode::OwnsGeometry);
    bars_->setMaterial(new QSGVertexColorMaterial());
    bars_->setFlag(QSGNode::OwnsMaterial);
    clip_->appendChildNode(bars_);
    gate_->appendChildNode(clip_);
    vertices_ = 0;
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

void LadderLayer::updateNodes(const ChartFrame& frame, LayerNodeContext&) {
    if (!gate_) return;
    const double opacity = drawing_ ? 1.0 : 0.0;
    if (gate_->opacity() != opacity) gate_->setOpacity(opacity);
    if (!drawing_) return;
    const auto& f = frame.context;
    const Key key{current_->serial, f.raster.topF, f.raster.rowPxF, f.raster.tick, f.raster.dpr,
                  f.itemBounds.x() + f.itemBounds.width(), f.rightGutterPx, f.surfaceBounds.top(),
                  f.surfaceBounds.height()};
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
    auto* geometry = bars_->geometry();
    auto* v = geometry->vertexDataAsColoredPoint2D();
    int count = 0;
    for (int i = first; peak > 0 && i <= last; ++i) {
        const auto& row = s.rows[size_t(i)];
        const double k = double(s.binLo + i);
        const float yTop = float(f.surfaceBounds.top() + cam.yDev((k + 1) * bin) / cam.dpr);
        const float yBottom = float(f.surfaceBounds.top() + cam.yDev(k * bin) / cam.dpr);
        if (row.bid > 0) writeBar(v + (count++) * kVerticesPerBar, mid - half * row.bid / peak, mid, yTop, yBottom, kBid);
        if (row.ask > 0) writeBar(v + (count++) * kVerticesPerBar, mid, mid + half * row.ask / peak, yTop, yBottom, kAsk);
    }
    const int used = count * kVerticesPerBar;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry->setVertexCount(used);
#else
    if (used < vertices_) std::memset(v + used, 0, size_t(vertices_ - used) * sizeof(*v));
#endif
    vertices_ = used;
    geometry->markVertexDataDirty();
    bars_->markDirty(QSGNode::DirtyGeometry);
    ++dirtyMarks_;
    ++rebuilds_;
}

void LadderLayer::afterRendering() {
    if (wantsRepublish_.exchange(false) && republish_) republish_();
}
} // namespace chart
