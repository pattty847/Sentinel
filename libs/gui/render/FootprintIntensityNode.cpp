#include "FootprintIntensityNode.hpp"

#include <QMatrix4x4>
#include <QVector4D>
#include <cstring>

namespace {
class FootprintIntensityShader final : public QSGMaterialShader {
public:
    FootprintIntensityShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/footprint_intensity.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/footprint_intensity.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<FootprintIntensityMaterial*>(newMaterial);
        QByteArray* data = state.uniformData();
        const int uniformSize = sizeof(float) * (16 + 4);
        if (data->size() != uniformSize) {
            data->resize(uniformSize);
        }

        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            memcpy(data->data(), matrix.constData(), 64);
            changed = true;
        }

        const QColor color = material->color();
        const float alpha = color.alphaF() * state.opacity();
        const QVector4D params(color.redF(), color.greenF(), color.blueF(), alpha);
        memcpy(data->data() + 64, &params, sizeof(QVector4D));
        changed = true;
        return changed;
    }
};
} // namespace

FootprintIntensityMaterial::FootprintIntensityMaterial() {
    setFlag(QSGMaterial::Blending, true);
}

QSGMaterialType* FootprintIntensityMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader* FootprintIntensityMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new FootprintIntensityShader();
}

int FootprintIntensityMaterial::compare(const QSGMaterial* other) const {
    const auto* rhs = static_cast<const FootprintIntensityMaterial*>(other);
    if (m_color.rgba() == rhs->m_color.rgba()) {
        return 0;
    }
    return m_color.rgba() < rhs->m_color.rgba() ? -1 : 1;
}

FootprintIntensityNode::FootprintIntensityNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

void FootprintIntensityNode::setRect(const QRectF& rect) {
    if (m_rect == rect) {
        return;
    }
    m_rect = rect;
    updateGeometry();
}

void FootprintIntensityNode::setColor(const QColor& color) {
    if (m_material.color() == color) {
        return;
    }
    m_material.setColor(color);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::updateGeometry() {
    auto* vertices = m_geometry.vertexDataAsTexturedPoint2D();
    if (m_rect.isNull() || m_rect.isEmpty()) {
        vertices[0].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[1].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[2].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[3].set(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        vertices[0].set(m_rect.left(), m_rect.top(), 0.0f, 0.0f);
        vertices[1].set(m_rect.left(), m_rect.bottom(), 0.0f, 1.0f);
        vertices[2].set(m_rect.right(), m_rect.top(), 1.0f, 0.0f);
        vertices[3].set(m_rect.right(), m_rect.bottom(), 1.0f, 1.0f);
    }
    markDirty(QSGNode::DirtyGeometry);
}
