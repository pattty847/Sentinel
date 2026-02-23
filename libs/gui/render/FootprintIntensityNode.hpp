#pragma once

#include <QColor>
#include <QByteArray>
#include <QRectF>
#include <QSize>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <mutex>
#include <vector>

class FootprintIntensityMaterial final : public QSGMaterial {
public:
    FootprintIntensityMaterial();

    QSGMaterialType* type() const override;
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override;

    void setTexture(QSGTexture* texture) { m_texture = texture; }
    void setNeutralColor(const QColor& color) { m_neutralColor = color; }
    void setBidColor(const QColor& color) { m_bidColor = color; }
    void setAskColor(const QColor& color) { m_askColor = color; }
    void setNeutralFloor(float floor) { m_neutralFloor = floor; }
    void setMagnitudeScale(float scale) { m_magnitudeScale = scale; }
    void setMagnitudeGamma(float gamma) { m_magnitudeGamma = gamma; }
    void setTimeOffset(float offset) { m_timeOffset = offset; }
    void enqueueColumn(int x, QByteArray data);
    void takePendingUploads(std::vector<std::pair<int, QByteArray>>& out);

    QSGTexture* texture() const { return m_texture; }
    QColor neutralColor() const { return m_neutralColor; }
    QColor bidColor() const { return m_bidColor; }
    QColor askColor() const { return m_askColor; }
    float neutralFloor() const { return m_neutralFloor; }
    float magnitudeScale() const { return m_magnitudeScale; }
    float magnitudeGamma() const { return m_magnitudeGamma; }
    float timeOffset() const { return m_timeOffset; }

private:
    QSGTexture* m_texture = nullptr;
    QColor m_neutralColor = QColor(44, 52, 62, 180);
    QColor m_bidColor = QColor(46, 182, 230, 255);
    QColor m_askColor = QColor(235, 92, 52, 255);
    float m_neutralFloor = 0.16f;
    float m_magnitudeScale = 1.0f;
    float m_magnitudeGamma = 0.9f;
    float m_timeOffset = 0.0f;
    std::mutex m_uploadMutex;
    std::vector<std::pair<int, QByteArray>> m_pendingUploads;
};

class FootprintIntensityNode final : public QSGGeometryNode {
public:
    FootprintIntensityNode();
    ~FootprintIntensityNode() override;

    void setRect(const QRectF& rect);
    void setSourceRect(const QRectF& rect);
    void setColor(const QColor& color);
    void setBidColor(const QColor& color);
    void setAskColor(const QColor& color);
    void setNeutralFloor(float floor);
    void setMagnitudeScale(float scale);
    void setMagnitudeGamma(float gamma);
    void setTimeOffset(float offset);
    void setTexture(QSGTexture* texture);
    void enqueueColumn(int x, QByteArray data);
    bool hasTexture() const { return m_material.texture() != nullptr; }
    QSize textureSize() const { return m_textureSize; }

private:
    void updateGeometry();

    QSGGeometry m_geometry;
    FootprintIntensityMaterial m_material;
    QRectF m_rect;
    QRectF m_sourceRect;
    QSize m_textureSize;
};
