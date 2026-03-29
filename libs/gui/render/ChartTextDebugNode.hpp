#pragma once

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <vector>

#include "ChartTextPrimitives.hpp"

class ChartTextDebugNode final : public QSGGeometryNode {
public:
    ChartTextDebugNode();
    void updateGeometry(const std::vector<ChartGlyphInstance>& glyphs);

private:
    QSGGeometry m_geometry;
    QSGVertexColorMaterial m_material;
    int m_capacityVertices = 0;
};
