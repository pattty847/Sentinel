#include "ChartTextNode.hpp"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <cstring>

namespace {
class ChartTextShader final : public QSGMaterialShader {
public:
    ChartTextShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/heatmap_msdf.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/heatmap_msdf.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<ChartTextMaterial*>(newMaterial);
        QByteArray* data = state.uniformData();
        const int uniformSize = sizeof(float) * (16 + 4 + 4);
        if (data->size() != uniformSize) {
            data->resize(uniformSize);
        }

        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(data->data(), matrix.constData(), 64);
            changed = true;
        }

        QVector4D color = material->color();
        color.setW(color.w() * state.opacity());
        std::memcpy(data->data() + 64, &color, sizeof(QVector4D));

        const QVector4D params(material->pxRange(), material->sdfBias(), material->distanceSign(), material->sprFloor());
        std::memcpy(data->data() + 80, &params, sizeof(QVector4D));
        changed = true;
        return changed;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<ChartTextMaterial*>(newMaterial);
        if (binding == 1) {
            *texture = material->texture();
        }
        if (*texture) {
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
    }
};
} // namespace

ChartTextMaterial::ChartTextMaterial() {
    setFlag(QSGMaterial::Blending, true);
}

QSGMaterialType* ChartTextMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader* ChartTextMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new ChartTextShader();
}

int ChartTextMaterial::compare(const QSGMaterial* other) const {
    const auto* rhs = static_cast<const ChartTextMaterial*>(other);
    if (m_texture != rhs->m_texture) {
        return m_texture < rhs->m_texture ? -1 : 1;
    }
    if (m_color != rhs->m_color) {
        if (m_color.x() != rhs->m_color.x()) return m_color.x() < rhs->m_color.x() ? -1 : 1;
        if (m_color.y() != rhs->m_color.y()) return m_color.y() < rhs->m_color.y() ? -1 : 1;
        if (m_color.z() != rhs->m_color.z()) return m_color.z() < rhs->m_color.z() ? -1 : 1;
        return m_color.w() < rhs->m_color.w() ? -1 : 1;
    }
    if (m_pxRange != rhs->m_pxRange) {
        return m_pxRange < rhs->m_pxRange ? -1 : 1;
    }
    return 0;
}

void ChartTextMaterial::setColor(const QColor& color) {
    m_color = QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

ChartTextNode::ChartTextNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangles);
    m_geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

ChartTextNode::~ChartTextNode() {
    delete m_texture;
    m_texture = nullptr;
}

void ChartTextNode::setAtlas(const QImage& image, QQuickWindow* window) {
    if (!window || image.isNull()) {
        return;
    }
    if (!m_texture || m_atlasSize != image.size()) {
        delete m_texture;
        m_texture = window->createTextureFromImage(image);
        if (m_texture) {
            m_texture->setFiltering(QSGTexture::Linear);
            m_texture->setMipmapFiltering(QSGTexture::None);
            m_texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
            m_texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
            if (m_texture->isAtlasTexture()) {
                QSGTexture* nonAtlas = m_texture->removedFromAtlas();
                if (nonAtlas && nonAtlas != m_texture) {
                    delete m_texture;
                    m_texture = nonAtlas;
                    m_texture->setFiltering(QSGTexture::Linear);
                    m_texture->setMipmapFiltering(QSGTexture::None);
                    m_texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
                    m_texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
                }
            }
            m_texSubRect = m_texture->normalizedTextureSubRect();
        }
        m_atlasSize = image.size();
        updateMaterial();
    }
}

void ChartTextNode::setColor(const QColor& color) {
    const QVector4D vec(color.redF(), color.greenF(), color.blueF(), color.alphaF());
    if (m_material.color() == vec) {
        return;
    }
    m_material.setColor(color);
    updateMaterial();
}

void ChartTextNode::setPxRange(float pxRange) {
    if (m_material.pxRange() == pxRange) {
        return;
    }
    m_material.setPxRange(pxRange);
    updateMaterial();
}

void ChartTextNode::setSdfBias(float sdfBias) {
    if (m_material.sdfBias() == sdfBias) {
        return;
    }
    m_material.setSdfBias(sdfBias);
    updateMaterial();
}

void ChartTextNode::setDistanceSign(float distanceSign) {
    if (m_material.distanceSign() == distanceSign) {
        return;
    }
    m_material.setDistanceSign(distanceSign);
    updateMaterial();
}

void ChartTextNode::setSprFloor(float sprFloor) {
    if (m_material.sprFloor() == sprFloor) {
        return;
    }
    m_material.setSprFloor(sprFloor);
    updateMaterial();
}

void ChartTextNode::updateGeometry(const std::vector<ChartGlyphInstance>& glyphs) {
    const int glyphCount = static_cast<int>(glyphs.size());
    if (glyphCount > m_capacityGlyphs) {
        m_capacityGlyphs = glyphCount;
        m_geometry.allocate(m_capacityGlyphs * 6);
    }

    auto* vertices = m_geometry.vertexDataAsTexturedPoint2D();
    int dst = 0;
    for (int i = 0; i < glyphCount; ++i) {
        const ChartGlyphInstance& glyph = glyphs[i];
        const float x0 = static_cast<float>(glyph.rect.left());
        const float y0 = static_cast<float>(glyph.rect.top());
        const float x1 = static_cast<float>(glyph.rect.right());
        const float y1 = static_cast<float>(glyph.rect.bottom());
        const float u0 = static_cast<float>(m_texSubRect.left() + glyph.uv.left() * m_texSubRect.width());
        const float v0 = static_cast<float>(m_texSubRect.top() + glyph.uv.top() * m_texSubRect.height());
        const float u1 = static_cast<float>(m_texSubRect.left() + glyph.uv.right() * m_texSubRect.width());
        const float v1 = static_cast<float>(m_texSubRect.top() + glyph.uv.bottom() * m_texSubRect.height());

        vertices[dst + 0].set(x0, y0, u0, v0);
        vertices[dst + 1].set(x0, y1, u0, v1);
        vertices[dst + 2].set(x1, y0, u1, v0);
        vertices[dst + 3].set(x1, y0, u1, v0);
        vertices[dst + 4].set(x0, y1, u0, v1);
        vertices[dst + 5].set(x1, y1, u1, v1);
        dst += 6;
    }
    const int totalVertices = m_capacityGlyphs * 6;
    if (dst < totalVertices) {
        std::memset(vertices + dst, 0,
                    sizeof(QSGGeometry::TexturedPoint2D) * (totalVertices - dst));
    }

    markDirty(QSGNode::DirtyGeometry);
}

void ChartTextNode::updateMaterial() {
    m_material.setTexture(m_texture);
    markDirty(QSGNode::DirtyMaterial);
}
