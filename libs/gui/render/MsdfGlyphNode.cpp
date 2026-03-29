/*
Sentinel — MsdfGlyphNode
*/
#include "MsdfGlyphNode.hpp"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <cstring>

namespace {
class MsdfGlyphShader final : public QSGMaterialShader {
public:
    MsdfGlyphShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/heatmap_msdf.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/heatmap_msdf.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<MsdfGlyphMaterial*>(newMaterial);
        QByteArray* data = state.uniformData();
        const int uniformSize = sizeof(float) * (16 + 4 + 4);
        if (data->size() != uniformSize) {
            data->resize(uniformSize);
        }

        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            memcpy(data->data(), matrix.constData(), 64);
            changed = true;
        }

        QVector4D color = material->color();
        color.setW(color.w() * state.opacity());
        memcpy(data->data() + 64, &color, sizeof(QVector4D));

        const QVector4D params(material->pxRange(), 0.0f, -1.0f, 2.0f);
        memcpy(data->data() + 80, &params, sizeof(QVector4D));
        changed = true;

        return changed;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<MsdfGlyphMaterial*>(newMaterial);
        if (binding == 1) {
            *texture = material->texture();
        }

        if (*texture) {
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
    }
};
} // namespace

MsdfGlyphMaterial::MsdfGlyphMaterial() {
    setFlag(QSGMaterial::Blending, true);
}

QSGMaterialType* MsdfGlyphMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader* MsdfGlyphMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new MsdfGlyphShader();
}

int MsdfGlyphMaterial::compare(const QSGMaterial* other) const {
    auto* rhs = static_cast<const MsdfGlyphMaterial*>(other);
    if (m_texture != rhs->m_texture) {
        return m_texture < rhs->m_texture ? -1 : 1;
    }
    if (m_color != rhs->m_color) {
        if (m_color.x() != rhs->m_color.x()) {
            return m_color.x() < rhs->m_color.x() ? -1 : 1;
        }
        if (m_color.y() != rhs->m_color.y()) {
            return m_color.y() < rhs->m_color.y() ? -1 : 1;
        }
        if (m_color.z() != rhs->m_color.z()) {
            return m_color.z() < rhs->m_color.z() ? -1 : 1;
        }
        return m_color.w() < rhs->m_color.w() ? -1 : 1;
    }
    if (m_pxRange != rhs->m_pxRange) {
        return m_pxRange < rhs->m_pxRange ? -1 : 1;
    }
    return 0;
}

void MsdfGlyphMaterial::setColor(const QColor& color) {
    m_color = QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

MsdfGlyphNode::MsdfGlyphNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangles);
    m_geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

MsdfGlyphNode::~MsdfGlyphNode() {
    delete m_texture;
    m_texture = nullptr;
}

void MsdfGlyphNode::setAtlas(const QImage& image, QQuickWindow* window) {
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

void MsdfGlyphNode::setColor(const QColor& color) {
    const QVector4D vec(color.redF(), color.greenF(), color.blueF(), color.alphaF());
    if (m_material.color() == vec) return;
    m_material.setColor(color);
    updateMaterial();
}

void MsdfGlyphNode::setPxRange(float pxRange) {
    if (m_material.pxRange() == pxRange) return;
    m_material.setPxRange(pxRange);
    updateMaterial();
}

void MsdfGlyphNode::ensureCapacity(int maxQuads) {
    if (maxQuads <= m_capacityQuads) {
        return;
    }
    m_capacityQuads = maxQuads;
    const int vertexCount = m_capacityQuads * 6;
    m_geometry.allocate(vertexCount);
    markDirty(QSGNode::DirtyGeometry);
}

void MsdfGlyphNode::updateGeometry(const std::vector<GlyphQuad>& quads) {
    if (m_capacityQuads <= 0) {
        return;
    }
    auto* vertices = m_geometry.vertexDataAsTexturedPoint2D();
    const int quadCount = static_cast<int>(quads.size());
    const int usedVertices = std::min(quadCount, m_capacityQuads) * 6;
    int dst = 0;
    for (int i = 0; i < quadCount && dst < usedVertices; ++i) {
        const auto& quad = quads[i];
        for (int v = 0; v < 6 && dst < usedVertices; ++v, ++dst) {
            const float u = static_cast<float>(m_texSubRect.left() +
                                               quad.uv[v].x() * m_texSubRect.width());
            const float vcoord = static_cast<float>(m_texSubRect.top() +
                                                    quad.uv[v].y() * m_texSubRect.height());
            vertices[dst].set(quad.pos[v].x(), quad.pos[v].y(),
                              u, vcoord);
        }
    }
    const int totalVertices = m_capacityQuads * 6;
    if (usedVertices < totalVertices) {
        std::memset(vertices + usedVertices, 0,
                    sizeof(QSGGeometry::TexturedPoint2D) * (totalVertices - usedVertices));
    }
    markDirty(QSGNode::DirtyGeometry);
}

void MsdfGlyphNode::updateMaterial() {
    m_material.setTexture(m_texture);
    markDirty(QSGNode::DirtyMaterial);
}
