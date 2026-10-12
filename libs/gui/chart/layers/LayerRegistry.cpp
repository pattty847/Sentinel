#include "chart/layers/LayerRegistry.hpp"
#include <QSGNode>

namespace chart {
void LayerRegistry::onRootRebuilt(QSGNode* root) {
    for (auto& entry : layers_) {
        entry.layer->onRootRebuilt();
        entry.container = new QSGNode();
        root->appendChildNode(entry.container);
    }
}
} // namespace chart
