#pragma once

#include <QColor>
#include <QRectF>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QVector4D>
#include <vector>

#include "ChartTextPrimitives.hpp"

class QQuickWindow;

class ChartTextMaterial final : public QSGMaterial {
public:
    ChartTextMaterial();

    QSGMaterialType* type() const override;
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override;

    void setTexture(QSGTexture* texture) { m_texture = texture; }
    void setColor(const QColor& color);
    void setPxRange(float pxRange) { m_pxRange = pxRange; }
    void setSdfBias(float sdfBias) { m_sdfBias = sdfBias; }
    void setDistanceSign(float distanceSign) { m_distanceSign = distanceSign; }
    void setSprFloor(float sprFloor) { m_sprFloor = sprFloor; }
    const QVector4D& color() const { return m_color; }
    float pxRange() const { return m_pxRange; }
    float sdfBias() const { return m_sdfBias; }
    float distanceSign() const { return m_distanceSign; }
    float sprFloor() const { return m_sprFloor; }
    QSGTexture* texture() const { return m_texture; }

private:
    QSGTexture* m_texture = nullptr;
    QVector4D m_color{1.0f, 1.0f, 1.0f, 1.0f};
    float m_pxRange = 4.0f;
    float m_sdfBias = 0.0f;
    float m_distanceSign = -1.0f;
    float m_sprFloor = 2.0f;
};

class ChartTextNode final : public QSGGeometryNode {
public:
    ChartTextNode();
    ~ChartTextNode() override;

    void setAtlas(const QImage& image, QQuickWindow* window);
    void setColor(const QColor& color);
    void setPxRange(float pxRange);
    void setSdfBias(float sdfBias);
    void setDistanceSign(float distanceSign);
    void setSprFloor(float sprFloor);
    void updateGeometry(const std::vector<ChartGlyphInstance>& glyphs);

private:
    void updateMaterial();

    QSGGeometry m_geometry;
    ChartTextMaterial m_material;
    QSGTexture* m_texture = nullptr;
    int m_capacityGlyphs = 0;
    QSize m_atlasSize;
    QRectF m_texSubRect{0.0, 0.0, 1.0, 1.0};
};
