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