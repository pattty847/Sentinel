/*
Sentinel — GridSceneNode
Role: A custom QSGNode that is the root of the chart's scene graph, owning all child nodes.
Inputs/Outputs: Provides methods to update content from a render strategy and transform matrix.
Threading: All methods are designed to be called only on the Qt Quick render thread.
Performance: Manages the lifecycle of child nodes, replacing old geometry with new.
Integration: Instantiated and controlled by UnifiedGridRenderer; parent to strategy-built nodes.
Observability: No internal logging.
Related: GridSceneNode.cpp, UnifiedGridRenderer.h, IRenderStrategy.hpp.
Assumptions: Assumes ownership of its child nodes and manages their memory.
*/
#pragma once
#include <QSGTransformNode>
#include <QSGGeometryNode>
#include <QMatrix4x4>
#include <memory>

class IRenderStrategy;
struct GridSliceBatch;

class GridSceneNode : public QSGTransformNode {
public:
    GridSceneNode();
    ~GridSceneNode() override;
    
    void updateContent(const GridSliceBatch& batch, IRenderStrategy* strategy);
    void updateTransform(const QMatrix4x4& transform);
    
    void setShowVolumeProfile(bool show);
    void updateVolumeProfile(const std::vector<std::pair<double, double>>& profile);
    
private:
    QSGNode* m_contentNode = nullptr;
    QSGNode* m_volumeProfileNode = nullptr;
    bool m_showVolumeProfile = true;
    
    QSGNode* createVolumeProfileNode(const std::vector<std::pair<double, double>>& profile);
};