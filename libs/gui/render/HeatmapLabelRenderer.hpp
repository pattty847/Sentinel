/*
Sentinel — HeatmapLabelRenderer
Role: Builds glyph quads for GPU label rendering.
Threading: Render thread only.
*/
#pragma once

#include <QRectF>
#include <QString>
#include <QVector2D>
#include <vector>

#include "HeatmapStreamState.hpp"
#include "MsdfAtlas.hpp"
#include "MsdfGlyphNode.hpp"
#include "TimeAxisMapping.hpp"

class HeatmapLabelRenderer {
public:
    using GlyphQuad = MsdfGlyphNode::GlyphQuad;

    static void buildLabelQuads(const TimeAxisMapping& mapping,
                                const HeatmapStreamState::Snapshot& snapshot,
                                const MsdfAtlas& atlas,
                                const std::vector<uint16_t>& liquidityRing,
                                const std::vector<uint16_t>& intensityRing,
                                const std::vector<double>& liquidityScales,
                                float scale,
                                bool dollars,
                                std::vector<GlyphQuad>& whiteQuads,
                                std::vector<GlyphQuad>& blackQuads,
                                int onlyColumn = -1);

private:
    static QString formatLiquidityLabel(double value, bool dollars);
};
