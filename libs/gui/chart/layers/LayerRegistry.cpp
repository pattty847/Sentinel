#include "chart/layers/LayerRegistry.hpp"
#include <QSGNode>

namespace chart {
QSGNode* IChartLayer::createContainer() { return new QSGNode(); }
void LayerRegistry::onRootRebuilt(QSGNode* root) {
    for (auto& entry : layers_) {
        entry.layer->onRootRebuilt();
        entry.container = entry.layer->createContainer();
        root->appendChildNode(entry.container);
    }
}
} // namespace chart
