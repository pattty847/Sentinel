#include "GridSceneNode.hpp"
#include "GridTypes.hpp"
#include "IRenderStrategy.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometry>

GridSceneNode::GridSceneNode() {
    setFlag(QSGNode::OwnedByParent);
}

GridSceneNode::~GridSceneNode() {
    // Qt handles child node cleanup automatically
}

void GridSceneNode::updateContent(const GridSliceBatch& batch, IRenderStrategy* strategy) {
    if (!strategy) return;
    
    // Remove old content node
    if (m_contentNode) {
        removeChildNode(m_contentNode);
        delete m_contentNode;
        m_contentNode = nullptr;
    }
    
    // Create new content using strategy
    m_contentNode = strategy->buildNode(batch);
    if (m_contentNode) {
        appendChildNode(m_contentNode);
    }
}

void GridSceneNode::updateTransform(const QMatrix4x4& transform) {
    setMatrix(transform);
    markDirty(QSGNode::DirtyMatrix);
}

void GridSceneNode::setShowVolumeProfile(bool show) {
    if (m_showVolumeProfile != show) {
        m_showVolumeProfile = show;
        
        if (m_volumeProfileNode) {
            m_volumeProfileNode->setFlag(QSGNode::OwnedByParent, false);
            removeChildNode(m_volumeProfileNode);
            
            if (!show) {
                delete m_volumeProfileNode;
                m_volumeProfileNode = nullptr;
            } else {
                appendChildNode(m_volumeProfileNode);
                m_volumeProfileNode->setFlag(QSGNode::OwnedByParent, true);
            }
        }
    }
}

void GridSceneNode::updateVolumeProfile(const std::vector<std::pair<double, double>>& profile) {
    if (!m_showVolumeProfile) return;
    
    // Remove old volume profile node
    if (m_volumeProfileNode) {
        removeChildNode(m_volumeProfileNode);
        delete m_volumeProfileNode;
        m_volumeProfileNode = nullptr;
    }
    
    // Create new volume profile node
    m_volumeProfileNode = createVolumeProfileNode(profile);
    if (m_volumeProfileNode) {
        appendChildNode(m_volumeProfileNode);
    }
}

QSGNode* GridSceneNode::createVolumeProfileNode(const std::vector<std::pair<double, double>>& profile) {
    if (profile.empty()) return nullptr;
    
    auto* node = new QSGGeometryNode;
    auto* material = new QSGVertexColorMaterial;
    material->setFlag(QSGMaterial::Blending);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    
    // Create geometry for volume profile bars (6 vertices per bar)
    int vertexCount = profile.size() * 6;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    
    // Fill vertex buffer with volume profile bars
    auto* vertices = static_cast<QSGGeometry::ColoredPoint2D*>(geometry->vertexData());
    int vertexIndex = 0;
    
    for (size_t i = 0; i < profile.size(); ++i) {
        double price = profile[i].first;
        double volume = profile[i].second;
        
        // Simple volume bar rendering (would need proper positioning)
        float barHeight = 20.0f; // Fixed height for now
        float barWidth = std::min(100.0f, static_cast<float>(volume * 0.01)); // Scale volume
        
        float left = 0.0f;
        float right = barWidth;
        float top = price - barHeight * 0.5f;
        float bottom = price + barHeight * 0.5f;
        
        // Gray color for volume profile
        int gray = 128;
        int alpha = 180;
        
        // Triangle 1: top-left, top-right, bottom-left
        vertices[vertexIndex++].set(left, top, gray, gray, gray, alpha);
        vertices[vertexIndex++].set(right, top, gray, gray, gray, alpha);
        vertices[vertexIndex++].set(left, bottom, gray, gray, gray, alpha);
        
        // Triangle 2: top-right, bottom-right, bottom-left
        vertices[vertexIndex++].set(right, top, gray, gray, gray, alpha);
        vertices[vertexIndex++].set(right, bottom, gray, gray, gray, alpha);
        vertices[vertexIndex++].set(left, bottom, gray, gray, gray, alpha);
    }
    
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}