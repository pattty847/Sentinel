#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "render/heatmap/HeatmapLabelLayout.hpp"
#include <atomic>
#include <functional>
#include <memory>

class QSGOpacityNode;
namespace heatmap::gpu { class HeatmapGpuLayer; class HeatmapTileNode; struct HeatmapTileStats; }
namespace chart {
class HeatmapLayer final : public IChartLayer {
public:
    HeatmapLayer();
    ~HeatmapLayer() override;
    void initialize();
    std::string_view name() const override { return "heatmap"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void afterRendering() override;
    void submitLabels(const ChartFrame&, LayerNodeContext&);
    bool labelsChangedSinceDraw() const;
    std::unique_ptr<heatmap::gpu::HeatmapGpuLayer> gpu;
    std::function<void()> queueUpdate, queueLabelCheck;
    const auto& labelLayout() const { return m_gpuLabels; }
    const auto& labelGlyphs() const { return m_heatmapLabelGlyphs; }
    uint64_t labelSerial() const { return m_gpuLabelSerial; }
private:
    std::shared_ptr<heatmap::gpu::HeatmapTileStats> tileStats_;
    QSGOpacityNode* gate_ = nullptr;
    heatmap::gpu::HeatmapTileNode* tile_ = nullptr;
    bool prepared_ = false;
    std::vector<ChartGlyphInstance> m_heatmapLabelGlyphs;
    heatmap::gpu::HeatmapLabelLayout m_gpuLabels;
    uint64_t m_gpuLabelSerial = 0;
    std::vector<uint8_t> m_gpuLabelColumns;
    std::atomic<uint64_t> m_gpuLabelSignature{0};
    std::atomic<bool> m_gpuLabelsIncomplete{false};
};
} // namespace chart
