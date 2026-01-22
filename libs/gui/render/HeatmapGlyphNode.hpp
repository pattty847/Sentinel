/*
Sentinel — HeatmapGlyphNode
Role: Renders glyph quads with a shared atlas texture.
Threading: Render thread only.
*/
#pragma once

#include <QColor>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QVector2D>
#include <QVector4D>
#include <QRectF>
#include <vector>

class QQuickWindow;

class HeatmapGlyphMaterial final : public QSGMaterial {
public:
    HeatmapGlyphMaterial();

    QSGMaterialType* type() const override;
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial* other) const override;

    void setTexture(QSGTexture* texture) { m_texture = texture; }
    void setColor(const QColor& color);
    const QVector4D& color() const { return m_color; }
    QSGTexture* texture() const { return m_texture; }

private:
    QSGTexture* m_texture = nullptr;
    QVector4D m_color{1.0f, 1.0f, 1.0f, 1.0f};
};

class HeatmapGlyphNode final : public QSGGeometryNode {
public:
    struct GlyphQuad {
        QVector2D pos[6];
        QVector2D uv[6];
    };

    HeatmapGlyphNode();
    ~HeatmapGlyphNode() override;
    void setAtlas(const QImage& image, QQuickWindow* window);
    void setColor(const QColor& color);
    void ensureCapacity(int maxQuads);
    void updateGeometry(const std::vector<GlyphQuad>& quads);

private:
    void updateMaterial();

    QSGGeometry m_geometry;
    HeatmapGlyphMaterial m_material;
    QSGTexture* m_texture = nullptr;
    int m_capacityQuads = 0;
    QSize m_atlasSize;
    QRectF m_texSubRect{0.0, 0.0, 1.0, 1.0};
};
