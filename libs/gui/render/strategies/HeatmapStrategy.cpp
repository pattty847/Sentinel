/*
Sentinel — HeatmapStrategy
Role: Implements the logic for rendering a liquidity heatmap from grid cell data.
Inputs/Outputs: Creates a QSGGeometryNode where each cell is a pair of colored triangles.
Threading: All code is executed on the Qt Quick render thread.
Performance: DOD: inline color calc, node reuse. Uses proven chunking for Windows/ANGLE 65k limit.
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
#include <QSGGeometry>
#include <algorithm>
#include <vector>

// DOD: Inline color calculation - avoids QColor heap allocation per cell
void HeatmapStrategy::calculateColorInline(double liquidity, bool isBid, double intensity,
                                           int& r, int& g, int& b, int& a) const {
    (void)liquidity;
    double alpha = std::min(intensity, 1.0);
    a = std::clamp(static_cast<int>(alpha * 255.0), 0, 255);
    
    if (isBid) {
        g = std::clamp(static_cast<int>(255.0 * intensity), 0, 255);
        r = 0;
        b = 0;
    } else {
        r = std::clamp(static_cast<int>(255.0 * intensity), 0, 255);
        g = 0;
        b = 0;
    }
}

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

    // Root container holding one or more geometry chunks
    auto* root = new QSGNode;

    int producedKeptCells = 0;
    int streamPos = 0; // relative to startIndex
    int totalVerticesDrawn = 0;

    const Viewport viewport = dataAccessor->getViewport();
    const double intensityScale = dataAccessor->getIntensityScale();

    while (producedKeptCells < keptCells) {
        const int remaining = keptCells - producedKeptCells;
        const int targetCells = std::min(kCellsPerChunk, remaining);

        // Collect up to targetCells that pass the filter for this chunk
        // This ensures we know EXACT count before allocating geometry
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

            // DOD: Inline color calculation (no QColor allocation)
            double scaledIntensity = calculateIntensity(cell.liquidity, intensityScale);
            int r, g, b, a;
            calculateColorInline(cell.liquidity, cell.isBid, scaledIntensity, r, g, b, a);

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

// DOD: Update existing node in-place when structure is compatible
bool HeatmapStrategy::updateNode(QSGNode* existingNode, const IDataAccessor* dataAccessor) {
    if (!existingNode || !dataAccessor) return false;
    
    auto cellsPtr = dataAccessor->getVisibleCells();
    if (!cellsPtr || cellsPtr->empty()) return false;
    
    const auto& cells = *cellsPtr;
    const int total = static_cast<int>(cells.size());
    const int cellCount = std::min(total, dataAccessor->getMaxCells());
    const int startIndex = std::max(0, total - cellCount);
    const double minVolume = dataAccessor->getMinVolumeFilter();
    const Viewport viewport = dataAccessor->getViewport();
    const double intensityScale = dataAccessor->getIntensityScale();
    
    // Count cells passing filter
    int keptCells = 0;
    for (int i = 0; i < cellCount; ++i) {
        if (cells[startIndex + i].liquidity >= minVolume) ++keptCells;
    }
    if (keptCells == 0) return false;
    
    // Check if we can reuse existing structure
    const int requiredChunks = (keptCells + kCellsPerChunk - 1) / kCellsPerChunk;
    const int existingChunks = existingNode->childCount();
    
    // Only reuse if chunk count matches exactly (conservative approach)
    if (existingChunks != requiredChunks) {
        return false; // Signal caller to rebuild
    }
    
    // Reuse existing nodes - update geometry in place
    int producedCells = 0;
    int streamPos = 0;
    QSGNode* childNode = existingNode->firstChild();
    
    while (childNode && producedCells < keptCells) {
        auto* geoNode = static_cast<QSGGeometryNode*>(childNode);
        
        const int remaining = keptCells - producedCells;
        const int targetCells = std::min(kCellsPerChunk, remaining);
        
        // Collect cells for this chunk (same proven logic as buildNode)
        std::vector<const CellInstance*> chunkCells;
        chunkCells.reserve(targetCells);
        
        for (; streamPos < cellCount && static_cast<int>(chunkCells.size()) < targetCells; ++streamPos) {
            const auto& c = cells[startIndex + streamPos];
            if (c.liquidity >= minVolume) {
                chunkCells.push_back(&c);
            }
        }
        
        if (chunkCells.empty()) break;
        
        const int vertexCount = static_cast<int>(chunkCells.size()) * kVertsPerCell;
        QSGGeometry* geometry = geoNode->geometry();
        
        // Reallocate geometry if size changed
        if (geometry->vertexCount() != vertexCount) {
            auto* newGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
            newGeometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geoNode->setGeometry(newGeometry);
            geoNode->setFlag(QSGNode::OwnsGeometry);
            geometry = newGeometry;
        }
        
        // Fill vertices
        auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
        int vertexIndex = 0;
        
        for (const CellInstance* cellPtr : chunkCells) {
            const auto& cell = *cellPtr;
            
            double scaledIntensity = calculateIntensity(cell.liquidity, intensityScale);
            int r, g, b, a;
            calculateColorInline(cell.liquidity, cell.isBid, scaledIntensity, r, g, b, a);
            
            QPointF topLeft = CoordinateSystem::worldToScreen(cell.timeStart_ms, cell.priceMax, viewport);
            QPointF bottomRight = CoordinateSystem::worldToScreen(cell.timeEnd_ms, cell.priceMin, viewport);
            
            const float left = static_cast<float>(topLeft.x());
            const float top = static_cast<float>(topLeft.y());
            const float right = static_cast<float>(bottomRight.x());
            const float bottom = static_cast<float>(bottomRight.y());
            
            vertices[vertexIndex++].set(left,  top,    r, g, b, a);
            vertices[vertexIndex++].set(right, top,    r, g, b, a);
            vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
            
            vertices[vertexIndex++].set(right, top,    r, g, b, a);
            vertices[vertexIndex++].set(right, bottom, r, g, b, a);
            vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
        }
        
        geoNode->markDirty(QSGNode::DirtyGeometry);
        producedCells += static_cast<int>(chunkCells.size());
        childNode = childNode->nextSibling();
    }
    
    return true;
}

QColor HeatmapStrategy::calculateColor(double liquidity, bool isBid, double intensity) const {
    // Keep for interface compatibility
    int r, g, b, a;
    calculateColorInline(liquidity, isBid, intensity, r, g, b, a);
    return QColor(r, g, b, a);
}
