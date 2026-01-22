/*
Sentinel — HeatmapGlyphNode
*/
#include "HeatmapGlyphNode.hpp"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <cstring>

namespace {
class HeatmapGlyphShader final : public QSGMaterialShader {
public:
    HeatmapGlyphShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/heatmap_glyph.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/heatmap_glyph.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<HeatmapGlyphMaterial*>(newMaterial);
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

        QVector4D color = material->color();
        color.setW(color.w() * state.opacity());
        memcpy(data->data() + 64, &color, sizeof(QVector4D));
        changed = true;

        return changed;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<HeatmapGlyphMaterial*>(newMaterial);
        if (binding == 1) {
            *texture = material->texture();
        }

        if (*texture) {
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }
    }
};
} // namespace

HeatmapGlyphMaterial::HeatmapGlyphMaterial() {
    setFlag(QSGMaterial::Blending, true);
}

QSGMaterialType* HeatmapGlyphMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader* HeatmapGlyphMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new HeatmapGlyphShader();
}

int HeatmapGlyphMaterial::compare(const QSGMaterial* other) const {
    auto* rhs = static_cast<const HeatmapGlyphMaterial*>(other);
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
    return 0;
}

void HeatmapGlyphMaterial::setColor(const QColor& color) {
    m_color = QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

HeatmapGlyphNode::HeatmapGlyphNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangles);
    m_geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

HeatmapGlyphNode::~HeatmapGlyphNode() {
    delete m_texture;
    m_texture = nullptr;
}

void HeatmapGlyphNode::setAtlas(const QImage& image, QQuickWindow* window) {
    if (!window || image.isNull()) {
        return;
    }
    if (!m_texture || m_atlasSize != image.size()) {
        delete m_texture;
        m_texture = window->createTextureFromImage(image);
        if (m_texture) {
            m_texture->setFiltering(QSGTexture::Nearest);
            if (m_texture->isAtlasTexture()) {
                QSGTexture* nonAtlas = m_texture->removedFromAtlas();
                if (nonAtlas && nonAtlas != m_texture) {
                    delete m_texture;
                    m_texture = nonAtlas;
                    m_texture->setFiltering(QSGTexture::Nearest);
                }
            }
            m_texSubRect = m_texture->normalizedTextureSubRect();
        }
        m_atlasSize = image.size();
        updateMaterial();
    }
}

void HeatmapGlyphNode::setColor(const QColor& color) {
    m_material.setColor(color);
    updateMaterial();
}

void HeatmapGlyphNode::ensureCapacity(int maxQuads) {
    if (maxQuads <= m_capacityQuads) {
        return;
    }
    m_capacityQuads = maxQuads;
    const int vertexCount = m_capacityQuads * 6;
    m_geometry.allocate(vertexCount);
    markDirty(QSGNode::DirtyGeometry);
}

void HeatmapGlyphNode::updateGeometry(const std::vector<GlyphQuad>& quads) {
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

void HeatmapGlyphNode::updateMaterial() {
    m_material.setTexture(m_texture);
    markDirty(QSGNode::DirtyMaterial);
}
