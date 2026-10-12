#pragma once
#include "chart/layers/IChartLayer.hpp"
#include <vector>

namespace chart {
// Registration is completed before the first root is built. Each layer receives
// its own container, so lazy child creation cannot change the paint order.
class LayerRegistry {
public:
    void add(IChartLayer& layer) { layers_.push_back({&layer, nullptr}); }
    void onRootRebuilt(QSGNode* root);
    void sync(const ChartFrame& frame) {
        for (auto& entry : layers_) entry.layer->sync(frame);
    }
    void updateNodes(const ChartFrame& frame, LayerNodeContext context) {
        for (auto& entry : layers_) {
            context.root = entry.container;
            entry.layer->updateNodes(frame, context);
        }
    }
    void afterRendering() {
        for (auto& entry : layers_) entry.layer->afterRendering();
    }
private:
    struct Entry { IChartLayer* layer; QSGNode* container; };
    std::vector<Entry> layers_;
};
} // namespace chart
