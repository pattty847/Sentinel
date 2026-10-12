#pragma once
#include <string_view>

class QQuickWindow;
class QSGNode;
class ChartTextRenderer;
class ChartTextAtlas;
class FrameProfiler;

namespace chart {
struct ChartFrame;
struct LayerNodeContext {
    QQuickWindow* window;
    QSGNode* root;
    ChartTextRenderer& text;
    const ChartTextAtlas& atlas;
    bool atlasReady;
    FrameProfiler* profiler = nullptr;
};

class IChartLayer {
public:
    virtual ~IChartLayer() = default;
    virtual std::string_view name() const = 0;
    virtual void onRootRebuilt() = 0;
    virtual void sync(const ChartFrame&) {}
    virtual void updateNodes(const ChartFrame&, LayerNodeContext&) = 0;
    virtual void afterRendering() {}
};
} // namespace chart
