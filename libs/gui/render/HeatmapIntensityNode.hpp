#pragma once

#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGRendererInterface>
#include <QByteArray>
#include <mutex>
#include <vector>
#include <QSGTexture>
#include <QRectF>
#include <QSize>

class HeatmapIntensityMaterial final : public QSGMaterial {
public:
    HeatmapIntensityMaterial();

    QSGMaterialType* type() const override;
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override;

    void setIntensityTexture(QSGTexture* texture) { m_intensityTexture = texture; }
    void setPaletteTexture(QSGTexture* texture) { m_paletteTexture = texture; }
    void setGamma(float gamma) { m_gamma = gamma; }
    void setContrast(float contrast) { m_contrast = contrast; }
    void setTimeOffset(float offset) { m_timeOffset = offset; }
    void setShaderFloor(float floor) { m_shaderFloor = floor; }
    void enqueueColumn(int x, QByteArray data);
    void takePendingUploads(std::vector<std::pair<int, QByteArray>>& out);

    QSGTexture* intensityTexture() const { return m_intensityTexture; }
    QSGTexture* paletteTexture() const { return m_paletteTexture; }
    float gamma() const { return m_gamma; }
    float contrast() const { return m_contrast; }
    float timeOffset() const { return m_timeOffset; }
    float shaderFloor() const { return m_shaderFloor; }

private:
    QSGTexture* m_intensityTexture = nullptr;
    QSGTexture* m_paletteTexture = nullptr;
    float m_gamma = 1.0f;
    float m_contrast = 1.0f;
    float m_timeOffset = 0.0f;
    float m_shaderFloor = 0.1f;
    std::mutex m_uploadMutex;
    std::vector<std::pair<int, QByteArray>> m_pendingUploads;
};

class HeatmapIntensityNode final : public QSGGeometryNode {
public:
    HeatmapIntensityNode();
    ~HeatmapIntensityNode() override;
    void setRect(const QRectF& rect);
    void setSourceRect(const QRectF& rect);
    QRectF getSourceRect() const { return m_sourceRect; }
    void setTextures(QSGTexture* intensity, QSGTexture* palette);
    void setGamma(float gamma);
    void setContrast(float contrast);
    void setTimeOffset(float offset);
    void setShaderFloor(float floor);
    void enqueueColumn(int x, QByteArray data);

private:
    void updateGeometry();

    QSGGeometry m_geometry;
    HeatmapIntensityMaterial m_material;
    QRectF m_rect;
    QRectF m_sourceRect;
    QSize m_textureSize;
};
