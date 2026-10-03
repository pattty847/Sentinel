#include "TradeBubbleNode.hpp"
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <cstring>
#include <algorithm>

namespace {
struct Vertex { float x, y, u, v, r, g, b, a; };
const QSGGeometry::AttributeSet& attributes() {
    static const QSGGeometry::Attribute a[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
        QSGGeometry::Attribute::create(1, 2, QSGGeometry::FloatType),
        QSGGeometry::Attribute::create(2, 4, QSGGeometry::FloatType)};
    static const QSGGeometry::AttributeSet set{3, sizeof(Vertex), a};
    return set;
}
class Shader final : public QSGMaterialShader {
public:
    Shader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/trade_bubble.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/trade_bubble.frag.qsb"));
    }
    bool updateUniformData(RenderState& state, QSGMaterial*, QSGMaterial*) override {
        auto* data = state.uniformData();
        if (state.isMatrixDirty()) std::memcpy(data->data(), state.combinedMatrix().constData(), 64);
        const float opacity = state.opacity();
        std::memcpy(data->data() + 64, &opacity, 4);
        return true;
    }
};
class Material final : public QSGMaterial {
public:
    Material() { setFlag(Blending); }
    QSGMaterialType* type() const override { static QSGMaterialType t; return &t; }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override { return new Shader; }
    int compare(const QSGMaterial*) const override { return 0; }
};
}
TradeBubbleNode::TradeBubbleNode()
    : geometry_(attributes(), int(trade_bubbles::Layout::MaxBubbles * 6)),
      clipGeometry_(QSGGeometry::defaultAttributes_Point2D(), 4) {
    geometry_.setDrawingMode(QSGGeometry::DrawTriangles);
    geometry_.setVertexDataPattern(QSGGeometry::DynamicPattern);
    // Qt 6.10 can change the draw count without changing capacity. Qt 6.9
    // (the Windows baseline) keeps unused triangles degenerate instead.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry_.setVertexCount(0);
#else
    std::memset(geometry_.vertexData(), 0, trade_bubbles::Layout::MaxBubbles * 6 * sizeof(Vertex));
#endif
    clipGeometry_.setDrawingMode(QSGGeometry::DrawTriangleStrip);
    clip_.setGeometry(&clipGeometry_);
    clip_.setIsRectangular(true);
    appendChildNode(&clip_);
    clip_.appendChildNode(&transform_);
    transform_.appendChildNode(&mesh_);
    mesh_.setGeometry(&geometry_);
    mesh_.setMaterial(new Material);
    mesh_.setFlag(OwnsMaterial);
}
void TradeBubbleNode::sync(const trade_bubbles::Tape& tape, const TimeAxisMapping& m,
                          bool enabled, double minNotional, QColor buy, QColor sell) {
    lastScanRows_ = rangeComparisons_ = 0;
    const auto window = enabled && m.valid ? tape.visible(m.viewStartMs, m.viewEndMs) : trade_bubbles::Window{};
    rangeComparisons_ = window.comparisons;
    const auto key = window.key();
    const auto scaleX = [](const TimeAxisMapping& map) { return map.drawRect.width() / (map.appendMs * map.srcRect.width()); };
    const auto scaleY = [](const TimeAxisMapping& map) { return map.drawRect.height() / (map.tickSize * map.srcRect.height()); };
    // Endpoint identities + count identify an unchanged range: the tape only
    // inserts rows and evicts its oldest row, never edits existing executions.
    // This also ignores updates outside the viewport and duplicate replay rows.
    if (tape_ == &tape && rebuildCount_ && key == windowKey_ && enabled_ == enabled && minNotional_ == minNotional &&
        buy_ == buy && sell_ == sell && mapping_.valid == m.valid && mapping_.drawRect == m.drawRect &&
        scaleX(mapping_) == scaleX(m) && scaleY(mapping_) == scaleY(m) &&
        mapping_.priceToScreenY(m.viewMinPrice) == m.priceToScreenY(m.viewMinPrice) &&
        mapping_.viewMinPrice == m.viewMinPrice && mapping_.viewMaxPrice == m.viewMaxPrice) {
        QMatrix4x4 translation;
        if (!window.rows.empty()) {
            const double anchor = double(window.rows.front().timeMs);
            translation.translate(float(m.timeToScreenX(anchor)-mapping_.timeToScreenX(anchor)), 0.0f);
        }
        if (translation != transform_.matrix()) transform_.setMatrix(translation);
        return;
    }
    tape_ = &tape;
    windowKey_ = key; enabled_ = enabled; minNotional_ = minNotional;
    mapping_ = m; buy_ = buy; sell_ = sell;
    transform_.setMatrix(QMatrix4x4{});
    clip_.setClipRect(m.drawRect);
    QSGGeometry::updateRectGeometry(&clipGeometry_, m.drawRect);
    clip_.markDirty(DirtyGeometry);
    const auto bubbles = enabled ? layout_.build(window.rows, m, minNotional) : std::span<const trade_bubbles::Bubble>{};
    lastScanRows_ = enabled ? layout_.scannedRows() : 0;
    ++rebuildCount_;
    auto* v = static_cast<Vertex*>(geometry_.vertexData());
    size_t count = 0;
    constexpr float corners[6][2] = {{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
    for (const auto& b : bubbles) {
        if (!(b.radius > 0)) continue;
        const auto c = b.side == AggressorSide::Buy ? buy : sell;
        constexpr float alpha = 0.60f;
        for (const auto& uv : corners) {
            const float x = b.x + uv[0] * b.radius;
            const float y = b.y + uv[1] * b.radius;
            v[count++] = {x, y, uv[0], uv[1],
                          float(c.redF())*alpha, float(c.greenF())*alpha, float(c.blueF())*alpha, alpha};
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry_.setVertexCount(int(count));
#else
    if (int(count) < usedVertexCount_)
        std::fill(v + count, v + usedVertexCount_, Vertex{});
#endif
    const bool drawn = count || usedVertexCount_;
    usedVertexCount_ = int(count);
    // A dirty mark after synchronization asks the window for another frame:
    // nothing drawn before or now leaves the scene graph untouched.
    if (!drawn) return;
    ++dirtyMarks_;
    geometry_.markVertexDataDirty();
    mesh_.markDirty(DirtyGeometry);
}

void TradeBubbleNode::clear() {
    enabled_ = false;
    tape_ = nullptr;
    if (!usedVertexCount_) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry_.setVertexCount(0);
#else
    std::fill_n(static_cast<Vertex*>(geometry_.vertexData()), usedVertexCount_, Vertex{});
#endif
    usedVertexCount_ = 0;
    ++dirtyMarks_;
    geometry_.markVertexDataDirty();
    mesh_.markDirty(DirtyGeometry);
}
