#pragma once

#include "chart/core/ChartPorts.hpp"

class UnifiedGridRenderer;

namespace heatmap::gpu {
class HeatmapGpuLayer;
}

namespace chart {

class HeatmapChartPort final : public IChartEnvironment, public ITickOracle, public IHeatmapViewSink {
public:
    HeatmapChartPort(UnifiedGridRenderer& item, heatmap::gpu::HeatmapGpuLayer& layer)
        : item_(item), layer_(layer) {}

    SurfaceFacts surface() const override;
    double drawnTick() const override;
    double priceScale() const override;
    bool manualTick() const override;
    bool hasCurrentResolution() const override;
    bool priceScaleCurrent() const override;
    SpanLimits limits() const override;
    int64_t liveAnchorMs() const override;
    int64_t liveOpenEndMs() const override;
    int64_t oldestAvailableMs() const override;
    double recentMidPrice() const override;
    std::optional<heatmap::gpu::ViewWindow> restView() const override;

    int64_t predictTickUnits(const heatmap::gpu::ViewWindow& view) const override;
    bool buildsTick(int64_t units, const heatmap::gpu::ViewWindow& view) const override;
    int64_t autoTickState() const override;
    heatmap::AutoTickParams autoTickParams() const override;
    bool setAutoPriceTick(std::optional<int64_t> units) override;

    void setView(const heatmap::gpu::ViewWindow& drawn, bool priceKnown) override;

private:
    UnifiedGridRenderer& item_;
    heatmap::gpu::HeatmapGpuLayer& layer_;
};

} // namespace chart
