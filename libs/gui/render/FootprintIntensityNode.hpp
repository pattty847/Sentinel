#pragma once

#include <QColor>
#include <QRectF>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGRendererInterface>

class FootprintIntensityMaterial final : public QSGMaterial {
public:
    FootprintIntensityMaterial();

    QSGMaterialType* type() const override;
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override;

    void setColor(const QColor& color) { m_color = color; }
    QColor color() const { return m_color; }

private:
    QColor m_color = QColor(48, 60, 72, 160);
};

class FootprintIntensityNode final : public QSGGeometryNode {
public:
    FootprintIntensityNode();

    void setRect(const QRectF& rect);
    void setColor(const QColor& color);

private:
    void updateGeometry();

    QSGGeometry m_geometry;
    FootprintIntensityMaterial m_material;
    QRectF m_rect;
};
