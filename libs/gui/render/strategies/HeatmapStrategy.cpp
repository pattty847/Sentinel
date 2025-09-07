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
#include "../../../core/SentinelLogging.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QImage>
#include <algorithm>
// #include <iostream>

bool HeatmapStrategy::setGrid(const LiquidityTimeSeriesEngine::DenseGrid& grid) {
    if (grid.cols <= 0 || grid.rows <= 0 || grid.bins.empty()) return false;
    m_texSize = QSize(grid.cols, grid.rows);
    // Normalize values for preview; simple min/max
    float vmin = std::numeric_limits<float>::infinity();
    float vmax = 0.0f;
    for (const auto& b : grid.bins) {
        if (!b.missing) {
            vmin = std::min(vmin, b.value);
            vmax = std::max(vmax, b.value);
        }
    }
    if (!std::isfinite(vmin) || vmax <= vmin) { vmin = 0.0f; vmax = 1.0f; }
    QImage img(m_texSize, QImage::Format_Grayscale8);
    for (int r = 0; r < grid.rows; ++r) {
        uchar* row = img.scanLine(r);
        for (int c = 0; c < grid.cols; ++c) {
            const auto& b = grid.bins[static_cast<size_t>(r * grid.cols + c)];
            float norm = b.missing ? 0.0f : (b.value - vmin) / (vmax - vmin);
            norm = std::clamp(norm, 0.0f, 1.0f);
            row[c] = static_cast<uchar>(norm * 255.0f);
        }
    }
    // TODO(Phase 3): Upload to QSGTexture using scenegraph context, double-buffer and swap
    // For now we just store preview intent
    return true;
}

void HeatmapStrategy::setPreviewTransform(const QMatrix4x4& transform) {
    m_previewTransform = transform;
    m_hasPreview = true;
}

QSGNode* HeatmapStrategy::buildNode(const GridSliceBatch& batch) {
    if (batch.cells.empty()) {
        sLog_Render("🎯 HEATMAP EXIT: Returning nullptr - batch is empty");
        return nullptr;
    }
    
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    int cellCount = std::min(static_cast<int>(batch.cells.size()), batch.maxCells);
    int vertexCount = cellCount * 6;
    
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    for (int i = 0; i < cellCount; ++i) {
        const auto& cell = batch.cells[i];
        if (cell.liquidity < batch.minVolumeFilter) continue;
        double scaledIntensity = calculateIntensity(cell.liquidity, batch.intensityScale);
        QColor color = calculateColor(cell.liquidity, cell.isBid, scaledIntensity);
        float left = cell.screenRect.left();
        float right = cell.screenRect.right();
        float top = cell.screenRect.top();
        float bottom = cell.screenRect.bottom();
        const int r = color.red();
        const int g = color.green();
        const int b = color.blue();
        const int a = std::clamp(static_cast<int>(color.alpha()), 0, 255);
        vertices[vertexIndex++].set(left,  top,    r, g, b, a);
        vertices[vertexIndex++].set(right, top,    r, g, b, a);
        vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
        vertices[vertexIndex++].set(right, top,    r, g, b, a);
        vertices[vertexIndex++].set(right, bottom, r, g, b, a);
        vertices[vertexIndex++].set(left,  bottom, r, g, b, a);
    }
    
    geometry->allocate(vertexIndex);
    node->markDirty(QSGNode::DirtyGeometry);
    
    return node;
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