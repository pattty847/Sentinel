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
    : geometry_(attributes(), int(trade_bubbles::Layout::MaxBubbles * 6)) {
    geometry_.setDrawingMode(QSGGeometry::DrawTriangles);
    geometry_.setVertexDataPattern(QSGGeometry::DynamicPattern);
    // Qt 6.10 can change the draw count without changing capacity. Qt 6.9
    // (the Windows baseline) keeps unused triangles degenerate instead.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry_.setVertexCount(0);
#else
    std::memset(geometry_.vertexData(), 0, trade_bubbles::Layout::MaxBubbles * 6 * sizeof(Vertex));
#endif
    setGeometry(&geometry_);
    setMaterial(new Material);
    setFlag(OwnsMaterial);
}
void TradeBubbleNode::sync(const trade_bubbles::Tape& tape, const TimeAxisMapping& m,
                          bool enabled, double minNotional, QColor buy, QColor sell) {
    if (revision_ == tape.revision() && enabled_ == enabled && minNotional_ == minNotional &&
        buy_ == buy && sell_ == sell && mapping_.valid == m.valid && mapping_.drawRect == m.drawRect &&
        mapping_.srcRect == m.srcRect && mapping_.dataStartMs == m.dataStartMs &&
        mapping_.dataMaxPrice == m.dataMaxPrice && mapping_.appendMs == m.appendMs &&
        mapping_.tickSize == m.tickSize && mapping_.viewStartMs == m.viewStartMs &&
        mapping_.viewEndMs == m.viewEndMs && mapping_.viewMinPrice == m.viewMinPrice &&
        mapping_.viewMaxPrice == m.viewMaxPrice) return;
    revision_ = tape.revision(); enabled_ = enabled; minNotional_ = minNotional;
    mapping_ = m; buy_ = buy; sell_ = sell;
    const auto bubbles = enabled ? layout_.build(tape.samples(), m, minNotional) : std::span<const trade_bubbles::Bubble>{};
    auto* v = static_cast<Vertex*>(geometry_.vertexData());
    size_t count = 0;
    constexpr float corners[6][2] = {{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
    for (const auto& b : bubbles) {
        if (!(b.radius > 0)) continue;
        const auto c = b.side == AggressorSide::Buy ? buy : sell;
        constexpr float alpha = 0.60f;
        for (const auto& uv : corners) {
            // Clip quads to the plot without moving the circle's centre/UV.
            const float x = float(std::clamp(double(b.x + uv[0] * b.radius), m.drawRect.left(), m.drawRect.right()));
            const float y = float(std::clamp(double(b.y + uv[1] * b.radius), m.drawRect.top(), m.drawRect.bottom()));
            v[count++] = {x, y, (x-b.x)/b.radius, (y-b.y)/b.radius,
                          float(c.redF())*alpha, float(c.greenF())*alpha, float(c.blueF())*alpha, alpha};
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    geometry_.setVertexCount(int(count));
#else
    if (int(count) < usedVertexCount_)
        std::fill(v + count, v + usedVertexCount_, Vertex{});
#endif
    usedVertexCount_ = int(count);
    geometry_.markVertexDataDirty();
    markDirty(DirtyGeometry);
}
