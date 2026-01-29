#include "HeatmapIntensityNode.hpp"
#include <QMatrix4x4>
#include <QVector4D>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtQuick/qsgtexture_platform.h>
#include <cstring>

namespace {
class HeatmapIntensityShader final : public QSGMaterialShader {
public:
    HeatmapIntensityShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/heatmap_intensity.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/heatmap_intensity.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<HeatmapIntensityMaterial*>(newMaterial);
        QByteArray* data = state.uniformData();
        const int uniformSize = sizeof(float) * (16 + 8);
        if (data->size() != uniformSize) {
            data->resize(uniformSize);
        }

        bool changed = false;
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            memcpy(data->data(), matrix.constData(), 64);
            changed = true;
        }

        const float opacity = state.opacity();
        const QVector4D params(opacity, material->gamma(), material->contrast(), material->timeOffset());
        const QVector4D params2(material->shaderFloor(), 0.0f, 0.0f, 0.0f);
        memcpy(data->data() + 64, &params, sizeof(QVector4D));
        memcpy(data->data() + 64 + sizeof(QVector4D), &params2, sizeof(QVector4D));
        changed = true;

        return changed;
    }

    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture,
                            QSGMaterial* newMaterial, QSGMaterial* oldMaterial) override {
        Q_UNUSED(oldMaterial);
        auto* material = static_cast<HeatmapIntensityMaterial*>(newMaterial);
        if (binding == 1) {
            *texture = material->intensityTexture();
        } else if (binding == 2) {
            *texture = material->paletteTexture();
        }

        if (*texture) {
            (*texture)->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        }

        if (binding == 1 && *texture) {
            std::vector<std::pair<int, QByteArray>> uploads;
            material->takePendingUploads(uploads);
            if (!uploads.empty()) {
                auto* glTex = (*texture)->nativeInterface<QNativeInterface::QSGOpenGLTexture>();
                if (glTex) {
                    auto* ctx = QOpenGLContext::currentContext();
                    auto* gl = ctx ? ctx->functions() : nullptr;
                    if (gl) {
                        gl->glBindTexture(GL_TEXTURE_2D, glTex->nativeTexture());
                        const int height = (*texture)->textureSize().height();
                        const int width = (*texture)->textureSize().width();
                        for (const auto& upload : uploads) {
                            const int x = upload.first;
                            if (x < 0 || x >= width) {
                                continue;
                            }
                            const int byteCount = upload.second.size();
                            if (byteCount == height) {
                                gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, height,
                                                    GL_RED, GL_UNSIGNED_BYTE,
                                                    upload.second.constData());
                            } else if (byteCount == height * 2) {
                                gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, height,
                                                    GL_RED, GL_UNSIGNED_SHORT,
                                                    upload.second.constData());
                            } else if (byteCount == height * 4) {
                                gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, 1, height,
                                                    GL_BGRA, GL_UNSIGNED_BYTE,
                                                    upload.second.constData());
                            }
                        }
                    }
                }
            }
        }
    }
};
} // namespace

HeatmapIntensityMaterial::HeatmapIntensityMaterial() {
    setFlag(QSGMaterial::Blending, true);
}

QSGMaterialType* HeatmapIntensityMaterial::type() const {
    static QSGMaterialType type;
    return &type;
}

QSGMaterialShader* HeatmapIntensityMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new HeatmapIntensityShader();
}

int HeatmapIntensityMaterial::compare(const QSGMaterial* other) const {
    auto* rhs = static_cast<const HeatmapIntensityMaterial*>(other);
    if (m_intensityTexture != rhs->m_intensityTexture) {
        return m_intensityTexture < rhs->m_intensityTexture ? -1 : 1;
    }
    if (m_paletteTexture != rhs->m_paletteTexture) {
        return m_paletteTexture < rhs->m_paletteTexture ? -1 : 1;
    }
    if (m_gamma != rhs->m_gamma) {
        return m_gamma < rhs->m_gamma ? -1 : 1;
    }
    if (m_contrast != rhs->m_contrast) {
        return m_contrast < rhs->m_contrast ? -1 : 1;
    }
    if (m_shaderFloor != rhs->m_shaderFloor) {
        return m_shaderFloor < rhs->m_shaderFloor ? -1 : 1;
    }
    return 0;
}

HeatmapIntensityNode::HeatmapIntensityNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4) {
    m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

void HeatmapIntensityNode::setRect(const QRectF& rect) {
    if (m_rect == rect) {
        return;
    }
    m_rect = rect;
    updateGeometry();
}

void HeatmapIntensityNode::setSourceRect(const QRectF& rect) {
    if (m_sourceRect == rect) {
        return;
    }
    m_sourceRect = rect;
    updateGeometry();
}

void HeatmapIntensityNode::setTextures(QSGTexture* intensity, QSGTexture* palette) {
    m_material.setIntensityTexture(intensity);
    m_material.setPaletteTexture(palette);
    if (intensity) {
        m_textureSize = intensity->textureSize();
        if (m_sourceRect.isNull()) {
            m_sourceRect = QRectF(0.0, 0.0, m_textureSize.width(), m_textureSize.height());
        }
    }
    markDirty(QSGNode::DirtyMaterial);
    updateGeometry();
}

void HeatmapIntensityNode::setGamma(float gamma) {
    m_material.setGamma(gamma);
    markDirty(QSGNode::DirtyMaterial);
}

void HeatmapIntensityNode::setContrast(float contrast) {
    m_material.setContrast(contrast);
    markDirty(QSGNode::DirtyMaterial);
}

void HeatmapIntensityNode::setTimeOffset(float offset) {
    m_material.setTimeOffset(offset);
    markDirty(QSGNode::DirtyMaterial);
}

void HeatmapIntensityNode::setShaderFloor(float floor) {
    m_material.setShaderFloor(floor);
    markDirty(QSGNode::DirtyMaterial);
}

void HeatmapIntensityMaterial::enqueueColumn(int x, QByteArray data) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    m_pendingUploads.emplace_back(x, std::move(data));
}

void HeatmapIntensityMaterial::takePendingUploads(std::vector<std::pair<int, QByteArray>>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

void HeatmapIntensityNode::enqueueColumn(int x, QByteArray data) {
    m_material.enqueueColumn(x, std::move(data));
    markDirty(QSGNode::DirtyMaterial);
}

void HeatmapIntensityNode::updateGeometry() {
    if (m_rect.isNull() || m_textureSize.isEmpty()) {
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

    auto* vertices = m_geometry.vertexDataAsTexturedPoint2D();
    vertices[0].set(m_rect.left(), m_rect.top(), u0, v0);
    vertices[1].set(m_rect.left(), m_rect.bottom(), u0, v1);
    vertices[2].set(m_rect.right(), m_rect.top(), u1, v0);
    vertices[3].set(m_rect.right(), m_rect.bottom(), u1, v1);

    markDirty(QSGNode::DirtyGeometry);
}
