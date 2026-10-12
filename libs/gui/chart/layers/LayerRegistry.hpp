#pragma once
#include "chart/layers/IChartLayer.hpp"
#include <vector>
#include "render/FrameProfiler.hpp"

namespace chart {
// Registration is completed before the first root is built. Each layer receives
// its own container, so lazy child creation cannot change the paint order.
class LayerRegistry {
public:
    void add(IChartLayer& layer, FrameProfiler::Stage stage = FrameProfiler::StageCount) {
        layers_.push_back({&layer, nullptr, stage});
    }
    void onRootRebuilt(QSGNode* root);
    void sync(const ChartFrame& frame) {
        for (auto& entry : layers_) entry.layer->sync(frame);
    }
    void updateNodes(const ChartFrame& frame, LayerNodeContext context) {
        for (auto& entry : layers_) {
            context.root = entry.container;
            entry.layer->updateNodes(frame, context);
            // Repeated Overlays marks report the sum across layers and retain the per-frame maximum.
            if (context.profiler && entry.stage != FrameProfiler::StageCount) context.profiler->mark(entry.stage);
        }
    }
    void afterRendering() {
        for (auto& entry : layers_) entry.layer->afterRendering();
    }
    bool precedes(std::string_view first, std::string_view second) const {
        bool foundFirst = false;
        for (const auto& entry : layers_) {
            if (entry.layer->name() == first) foundFirst = true;
            if (entry.layer->name() == second) return foundFirst;
        }
        return false;
    }
private:
    struct Entry { IChartLayer* layer; QSGNode* container; FrameProfiler::Stage stage; };
    std::vector<Entry> layers_;
};
} // namespace chart
