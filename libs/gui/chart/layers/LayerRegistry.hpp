#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/FrameProfiler.hpp"
#include <vector>
#include <array>
#include <memory>
#include <QMetaObject>
#include "chart/layers/ChartStack.hpp"

class QQuickItem;

namespace chart {
// Registration is completed before the first root is built. Each layer receives
// its own ordered container; a layer may defer it until enabled.
struct LayerOutput;
class LayerOutputItem;
class LayerRegistry {
public:
    LayerRegistry();
    ~LayerRegistry();
    LayerRegistry(const LayerRegistry&) = delete;
    LayerRegistry& operator=(const LayerRegistry&) = delete;
    void bindOutputs(QQuickItem& chart);
    QSGNode* createRoot(QSGNode* oldRoot);
    QSGNode* createOutputRoot(LayerPlane plane, QSGNode* oldRoot = nullptr);
    void add(IChartLayer& layer, FrameProfiler::Stage stage = FrameProfiler::StageCount,
             LayerPlane plane = LayerPlane::Fields) {
        layers_.push_back({&layer, nullptr, stage, plane});
    }
    void sync(const ChartFrame& frame) {
        for (auto& entry : layers_) entry.layer->sync(frame);
    }
    void updateNodes(const ChartFrame& frame, LayerNodeContext context) {
        for (auto& entry : layers_) {
            ensureContainer(entry);
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
    struct Entry { IChartLayer* layer; QSGNode* container; FrameProfiler::Stage stage; LayerPlane plane; };
    void onRootRebuilt(QSGNode* root);
    void resetOutputs();
    QSGNode* parentFor(const Entry&) const;
    std::array<std::shared_ptr<LayerOutput>, 2> outputs_;
    std::array<LayerOutputItem*, 2> items_{};
    QMetaObject::Connection windowChanged_;
    void ensureContainer(Entry&);
    QSGNode* root_ = nullptr;
    std::vector<Entry> layers_;
};
} // namespace chart
