#include "ChartTextDebugNode.hpp"

#include <QSGGeometry>
#include <QtGlobal>
#include <cmath>
#include <cstring>

namespace {
constexpr float kAnchorRadius = 2.5f;

inline void setColoredVertex(QSGGeometry::ColoredPoint2D& vertex,
                             float x,
                             float y,
                             const QColor& color) {
    vertex.set(x, y, color.red(), color.green(), color.blue(), color.alpha());
}
} // namespace

ChartTextDebugNode::ChartTextDebugNode()
    : m_geometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0) {
    m_geometry.setDrawingMode(QSGGeometry::DrawLines);
    m_geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
    setGeometry(&m_geometry);
    setMaterial(&m_material);
}

void ChartTextDebugNode::updateGeometry(const std::vector<ChartGlyphInstance>& glyphs) {
    const bool drawDebug = qEnvironmentVariableIsSet("SENTINEL_CHART_TEXT_DEBUG_OVERLAY");
    const int verticesPerGlyph = 14;
    const int vertexCount = drawDebug ? static_cast<int>(glyphs.size()) * verticesPerGlyph : 0;
    if (vertexCount > m_capacityVertices) {
        m_capacityVertices = vertexCount;
        m_geometry.allocate(m_capacityVertices);
    }

    auto* vertices = m_geometry.vertexDataAsColoredPoint2D();
    int dst = 0;
    if (drawDebug) {
        const QColor rectColor(0, 255, 128, 190);
        const QColor baselineColor(255, 192, 0, 220);
        const QColor anchorColor(255, 64, 64, 220);
        for (const ChartGlyphInstance& glyph : glyphs) {
            const float x0 = static_cast<float>(glyph.rect.left());
            const float y0 = static_cast<float>(glyph.rect.top());
            const float x1 = static_cast<float>(glyph.rect.right());
            const float y1 = static_cast<float>(glyph.rect.bottom());

            setColoredVertex(vertices[dst + 0], x0, y0, rectColor);
            setColoredVertex(vertices[dst + 1], x1, y0, rectColor);
            setColoredVertex(vertices[dst + 2], x1, y0, rectColor);
            setColoredVertex(vertices[dst + 3], x1, y1, rectColor);
            setColoredVertex(vertices[dst + 4], x1, y1, rectColor);
            setColoredVertex(vertices[dst + 5], x0, y1, rectColor);
            setColoredVertex(vertices[dst + 6], x0, y1, rectColor);
            setColoredVertex(vertices[dst + 7], x0, y0, rectColor);

            const float baselineY = std::isfinite(glyph.debugBaselineY)
                ? glyph.debugBaselineY
                : (y0 + y1) * 0.5f;
            setColoredVertex(vertices[dst + 8], x0, baselineY, baselineColor);
            setColoredVertex(vertices[dst + 9], x1, baselineY, baselineColor);

            if (glyph.debugShowAnchor) {
                const float ax = static_cast<float>(glyph.debugAnchor.x());
                const float ay = static_cast<float>(glyph.debugAnchor.y());
                setColoredVertex(vertices[dst + 10], ax - kAnchorRadius, ay, anchorColor);
                setColoredVertex(vertices[dst + 11], ax + kAnchorRadius, ay, anchorColor);
                setColoredVertex(vertices[dst + 12], ax, ay - kAnchorRadius, anchorColor);
                setColoredVertex(vertices[dst + 13], ax, ay + kAnchorRadius, anchorColor);
            } else {
                std::memset(vertices + dst + 10, 0, sizeof(QSGGeometry::ColoredPoint2D) * 4);
            }
            dst += verticesPerGlyph;
        }
    }

    if (dst < m_capacityVertices) {
        std::memset(vertices + dst, 0,
                    sizeof(QSGGeometry::ColoredPoint2D) * (m_capacityVertices - dst));
    }

    markDirty(QSGNode::DirtyGeometry);
    markDirty(QSGNode::DirtyMaterial);
}
