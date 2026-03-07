#include "FootprintIntensityNode.hpp"

#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVector4D>
#include <QtQuick/qsgtexture_platform.h>
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
        const int uniformSize = sizeof(float) * (16 + 16);
        if (data->size() != uniformSize) {
            data->resize(uniformSize);
        }

        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            memcpy(data->data(), matrix.constData(), 64);
            changed = true;
        }

        const QColor neutral = material->neutralColor();
        const QColor bid = material->bidColor();
        const QColor ask = material->askColor();
        const QVector4D neutralColor(neutral.redF(), neutral.greenF(), neutral.blueF(), neutral.alphaF() * state.opacity());
        const QVector4D bidColor(bid.redF(), bid.greenF(), bid.blueF(), 1.0f);
        const QVector4D askColor(ask.redF(), ask.greenF(), ask.blueF(), 1.0f);
        const QVector4D tuning(material->neutralFloor(),
                               material->magnitudeScale(),
                               material->magnitudeGamma(),
                               material->timeOffset());
        memcpy(data->data() + 64, &neutralColor, sizeof(QVector4D));
        memcpy(data->data() + 64 + sizeof(QVector4D), &bidColor, sizeof(QVector4D));
        memcpy(data->data() + 64 + sizeof(QVector4D) * 2, &askColor, sizeof(QVector4D));
        memcpy(data->data() + 64 + sizeof(QVector4D) * 3, &tuning, sizeof(QVector4D));
        changed = true;
        return changed;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<FootprintIntensityMaterial*>(newMaterial);
        if (binding != 1) {
            return;
        }
        *texture = material->texture();
        if (!*texture) {
            return;
        }

        (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());

        std::vector<std::pair<int, QByteArray>> uploads;
        material->takePendingUploads(uploads);
        if (uploads.empty()) {
            return;
        }

        auto* glTex = (*texture)->nativeInterface<QNativeInterface::QSGOpenGLTexture>();
        if (!glTex) {
            return;
        }
        auto* ctx = QOpenGLContext::currentContext();
        auto* gl = ctx ? ctx->functions() : nullptr;
        if (!gl) {
            return;
        }

        const int width = (*texture)->textureSize().width();
        const int height = (*texture)->textureSize().height();
        const int expectedBytes = height * static_cast<int>(sizeof(uint16_t));
        gl->glBindTexture(GL_TEXTURE_2D, glTex->nativeTexture());
        for (const auto& upload : uploads) {
            const int x = upload.first;
            if (x < 0 || x >= width || upload.second.size() != expectedBytes) {
                continue;
            }
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, height,
                                GL_RED, GL_UNSIGNED_SHORT,
                                upload.second.constData());
        }
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
    if (m_texture != rhs->m_texture) {
        return m_texture < rhs->m_texture ? -1 : 1;
    }
    if (m_neutralColor.rgba() != rhs->m_neutralColor.rgba()) {
        return m_neutralColor.rgba() < rhs->m_neutralColor.rgba() ? -1 : 1;
    }
    if (m_bidColor.rgba() != rhs->m_bidColor.rgba()) {
        return m_bidColor.rgba() < rhs->m_bidColor.rgba() ? -1 : 1;
    }
    if (m_askColor.rgba() != rhs->m_askColor.rgba()) {
        return m_askColor.rgba() < rhs->m_askColor.rgba() ? -1 : 1;
    }
    if (m_neutralFloor != rhs->m_neutralFloor) {
        return m_neutralFloor < rhs->m_neutralFloor ? -1 : 1;
    }
    if (m_magnitudeScale != rhs->m_magnitudeScale) {
        return m_magnitudeScale < rhs->m_magnitudeScale ? -1 : 1;
    }
    if (m_magnitudeGamma != rhs->m_magnitudeGamma) {
        return m_magnitudeGamma < rhs->m_magnitudeGamma ? -1 : 1;
    }
    if (m_timeOffset != rhs->m_timeOffset) {
        return m_timeOffset < rhs->m_timeOffset ? -1 : 1;
    }
    return 0;
}

void FootprintIntensityMaterial::enqueueColumn(int x, QByteArray data) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    m_pendingUploads.emplace_back(x, std::move(data));
}

void FootprintIntensityMaterial::takePendingUploads(std::vector<std::pair<int, QByteArray>>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

FootprintIntensityNode::FootprintIntensityNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

FootprintIntensityNode::~FootprintIntensityNode() {
    delete m_material.texture();
    m_material.setTexture(nullptr);
}

void FootprintIntensityNode::setRect(const QRectF& rect) {
    if (m_rect == rect) {
        return;
    }
    m_rect = rect;
    updateGeometry();
}

void FootprintIntensityNode::setSourceRect(const QRectF& rect) {
    if (m_sourceRect == rect) {
        return;
    }
    m_sourceRect = rect;
    updateGeometry();
}

void FootprintIntensityNode::setColor(const QColor& color) {
    if (m_material.neutralColor() == color) {
        return;
    }
    m_material.setNeutralColor(color);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setBidColor(const QColor& color) {
    if (m_material.bidColor() == color) {
        return;
    }
    m_material.setBidColor(color);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setAskColor(const QColor& color) {
    if (m_material.askColor() == color) {
        return;
    }
    m_material.setAskColor(color);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setNeutralFloor(float floor) {
    if (m_material.neutralFloor() == floor) {
        return;
    }
    m_material.setNeutralFloor(floor);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setMagnitudeScale(float scale) {
    if (m_material.magnitudeScale() == scale) {
        return;
    }
    m_material.setMagnitudeScale(scale);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setMagnitudeGamma(float gamma) {
    if (m_material.magnitudeGamma() == gamma) {
        return;
    }
    m_material.setMagnitudeGamma(gamma);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setTimeOffset(float offset) {
    if (m_material.timeOffset() == offset) {
        return;
    }
    m_material.setTimeOffset(offset);
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::setTexture(QSGTexture* texture) {
    QSGTexture* old = m_material.texture();
    if (old && old != texture) {
        delete old;
    }
    m_material.setTexture(texture);
    m_textureSize = texture ? texture->textureSize() : QSize();
    if (m_textureSize.isEmpty()) {
        m_sourceRect = QRectF();
    } else if (m_sourceRect.isNull() || m_sourceRect.isEmpty()) {
        m_sourceRect = QRectF(0.0, 0.0, m_textureSize.width(), m_textureSize.height());
    }
    markDirty(QSGNode::DirtyMaterial);
    updateGeometry();
}

void FootprintIntensityNode::enqueueColumn(int x, QByteArray data) {
    m_material.enqueueColumn(x, std::move(data));
    markDirty(QSGNode::DirtyMaterial);
}

void FootprintIntensityNode::updateGeometry() {
    auto* vertices = m_geometry.vertexDataAsTexturedPoint2D();
    if (m_rect.isNull() || m_rect.isEmpty() || m_textureSize.isEmpty() || m_sourceRect.isEmpty()) {
        vertices[0].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[1].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[2].set(0.0f, 0.0f, 0.0f, 0.0f);
        vertices[3].set(0.0f, 0.0f, 0.0f, 0.0f);
        markDirty(QSGNode::DirtyGeometry);
        return;
    }

    const float texW = static_cast<float>(m_textureSize.width());
    const float texH = static_cast<float>(m_textureSize.height());
    if (texW <= 0.0f || texH <= 0.0f) {
        return;
    }
    const float u0 = static_cast<float>(m_sourceRect.x()) / texW;
    const float v0 = static_cast<float>(m_sourceRect.y()) / texH;
    const float u1 = static_cast<float>(m_sourceRect.x() + m_sourceRect.width()) / texW;
    const float v1 = static_cast<float>(m_sourceRect.y() + m_sourceRect.height()) / texH;

    vertices[0].set(m_rect.left(), m_rect.top(), u0, v0);
    vertices[1].set(m_rect.left(), m_rect.bottom(), u0, v1);
    vertices[2].set(m_rect.right(), m_rect.top(), u1, v0);
    vertices[3].set(m_rect.right(), m_rect.bottom(), u1, v1);
    markDirty(QSGNode::DirtyGeometry);
}
