#include "chart/layers/LayerRegistry.hpp"
#include <QSGNode>

namespace chart {
QSGNode* IChartLayer::createContainer() { return new QSGNode(); }
void LayerRegistry::onRootRebuilt(QSGNode* root) {
    root_ = root;
    for (auto& entry : layers_) {
        entry.layer->onRootRebuilt();
        entry.container = entry.layer->createContainer();
        if (entry.container) root->appendChildNode(entry.container);
    }
}
void LayerRegistry::ensureContainer(Entry& entry) {
    if (entry.container || !root_) return;
    entry.container = entry.layer->createContainer();
    if (!entry.container) return;
    // A lazy layer takes its registered position even when later layers exist.
    bool after = false;
    for (auto& next : layers_) {
        if (after && next.container) {
            root_->insertChildNodeBefore(entry.container, next.container);
            return;
        }
        if (&next == &entry) after = true;
    }
    root_->appendChildNode(entry.container);
}
} // namespace chart
