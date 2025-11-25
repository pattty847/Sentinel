/*
Sentinel — HeatmapStrategy
Role: Implements the logic for rendering a liquidity heatmap from grid cell data.
Inputs/Outputs: Creates a QSGGeometryNode where each cell is a pair of colored triangles.
Threading: All code is executed on the Qt Quick render thread.
Performance: Uses a QSGVertexColorMaterial for efficient rendering of per-vertex colored geometry.
Integration: The concrete implementation of the heatmap visualization strategy.
Observability: No internal logging.
Related: HeatmapStrategy.hpp.
Assumptions: Liquidity intensity is represented by the alpha channel of the vertex color.
*/
#include "HeatmapStrategy.hpp"
#include "../GridTypes.hpp"
#include "../../CoordinateSystem.h"
#include "../../../core/SentinelLogging.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <algorithm>
#include <vector>
// #include <iostream>


QSGNode* HeatmapStrategy::buildNode(const IDataAccessor* dataAccessor) {

    /*
        Build a GPU scene graph node for rendering the heatmap. 
        Take our CellInstance data and convert them to colored triangles in world space.
    */

    if (!dataAccessor) return nullptr;
    auto cellsPtr = dataAccessor->getVisibleCells();
    if (!cellsPtr || cellsPtr->empty()) {
        sLog_Render(" HEATMAP EXIT: Returning nullptr - batch is empty");
        return nullptr;
    }

    // Calculate required vertex count window (6 vertices per cell for 2 triangles)
    const auto& cells = *cellsPtr;
    int total = static_cast<int>(cells.size());
    int cellCount = std::min(total, dataAccessor->getMaxCells());
    int startIndex = std::max(0, total - cellCount); // keep newest when clipping

    // First pass: count how many cells pass the min volume filter
    int keptCells = 0;
    const double minVolume = dataAccessor->getMinVolumeFilter();
    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = cells[startIndex + i];
        if (cell.liquidity >= minVolume) {
            ++keptCells;
        }
    }
    if (keptCells == 0) {
        sLog_Render(" HEATMAP EXIT: No cells above minVolumeFilter");
        return nullptr;
    }

    // Windows/ANGLE can enforce 16-bit index limits. Keep each geometry node under
    // a safe vertex threshold to avoid wrapping/overpaint artifacts.
    static constexpr int kMaxVerticesPerNode = 60000; // safety margin under 65535
    static constexpr int kVertsPerCell = 6;
    const int cellsPerChunk = std::max(1, kMaxVerticesPerNode / kVertsPerCell);

    // Root container holding one or more geometry chunks
    auto* root = new QSGNode;

    int producedKeptCells = 0;
    int streamPos = 0; // relative to startIndex
    int totalVerticesDrawn = 0;

    const Viewport viewport = dataAccessor->getViewport();
    const double intensityScale = dataAccessor->getIntensityScale();

    while (producedKeptCells < keptCells) {
        const int remaining = keptCells - producedKeptCells;
        const int targetCells = std::min(cellsPerChunk, remaining);

        // Collect up to targetCells that pass the filter for this chunk
        std::vector<const CellInstance*> chunkCells;
        chunkCells.reserve(targetCells);

        for (; streamPos < cellCount && static_cast<int>(chunkCells.size()) < targetCells; ++streamPos) {
            const auto& c = cells[startIndex + streamPos];
            if (c.liquidity >= minVolume) {
                chunkCells.push_back(&c);
            }
        }

        if (chunkCells.empty()) {
            break; // safety; shouldn't happen given keptCells accounting
        }

        const int vertexCount = static_cast<int>(chunkCells.size()) * kVertsPerCell;

        auto* node = new QSGGeometryNode;
        auto* material = new QSGVertexColorMaterial;
        material->setFlag(QSGMaterial::Blending);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);

        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);

        auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
        int vertexIndex = 0;

        for (const CellInstance* cellPtr : chunkCells) {
            const auto& cell = *cellPtr;

            // Color with intensity scaling
            double scaledIntensity = calculateIntensity(cell.liquidity, intensityScale);
            QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
            const int r = color.red();
            const int g = color.green();
            const int b = color.blue();
            const int a = std::clamp(static_cast<int>(color.alpha()), 0, 255);

            // Convert world→screen using batch.viewport
            QPointF topLeft = CoordinateSystem::worldToScreen(cell.timeStart_ms, cell.priceMax, viewport);
            QPointF bottomRight = CoordinateSystem::worldToScreen(cell.timeEnd_ms, cell.priceMin, viewport);

            const float left = static_cast<float>(topLeft.x());
            const float top = static_cast<float>(topLeft.y());
            const float right = static_cast<float>(bottomRight.x());
            const float bottom = static_cast<float>(bottomRight.y());

            // Triangle 1: top-left, top-right, bottom-left
            vertices[vertexIndex++].set(left,  top,    r, g, b, a);
            vertices[vertexIndex++].set(right, top,    r, g, b, a);
            vertices[vertexIndex++].set(left,  bottom, r, g, b, a);

            // Triangle 2: top-right, bottom-right, bottom-left
            vertices[vertexIndex++].set(right, top,    r, g, b, a);
            vertices[vertexIndex++].set(right, bottom, r, g, b, a);
            vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
        }

        node->markDirty(QSGNode::DirtyGeometry);
        root->appendChildNode(node);

        producedKeptCells += static_cast<int>(chunkCells.size());
        totalVerticesDrawn += vertexCount;
    }

    // HEATMAP CHUNK LOGGING (throttled)
    static int frame = 0;
    if ((++frame % 30) == 0) {
        sLog_RenderN(1, " HEATMAP CHUNKS: cells=" << keptCells
                         << " verts=" << totalVerticesDrawn
                         << " chunks=" << root->childCount());
    }

    return root;
}

QColor HeatmapStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    double alpha = std::min(intensity, 1.0); // LINUS FIX: let intensity speak
    
    if (isBid) {
        // Bid-heavy: Green spectrum
        double green = std::min(255.0 * intensity, 255.0);
        return QColor(0, static_cast<int>(green), 0, static_cast<int>(alpha * 255));
    } else {
        // Ask-heavy: Red spectrum  
        double red = std::min(255.0 * intensity, 255.0);
        return QColor(static_cast<int>(red), 0, 0, static_cast<int>(alpha * 255));
    }
}
