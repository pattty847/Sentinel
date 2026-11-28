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

// Render labels with MSDF text or cached glyph runs, never per-cell QML Text
// objects or delegate rebuilds during interaction.
class IChartLayer {
public:
    virtual ~IChartLayer() = default;
    virtual std::string_view name() const = 0;
    virtual void onRootRebuilt() = 0;
    // Return nullptr to defer the container; the registry retries after sync.
    virtual QSGNode* createContainer();
    // Keep pan/zoom GPU-first; update retained scene-graph data without QML object churn.
    virtual void sync(const ChartFrame&) {}
    // Reject NaN/inf before writing GPU geometry.
    // Preallocate and reuse geometry/nodes; avoid one child node per cell or trade.
    virtual void updateNodes(const ChartFrame&, LayerNodeContext&) = 0;
    virtual void afterRendering() {}
};
} // namespace chart
