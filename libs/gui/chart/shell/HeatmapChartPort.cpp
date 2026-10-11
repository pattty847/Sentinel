#include "chart/shell/HeatmapChartPort.hpp"

#include "render/heatmap/HeatmapGpuLayer.hpp"

namespace chart {

double HeatmapChartPort::drawnTick() const { return layer_.tickPrice(); }
double HeatmapChartPort::priceScale() const { return layer_.priceScale(); }
bool HeatmapChartPort::manualTick() const { return layer_.manualMode(); }
bool HeatmapChartPort::hasCurrentResolution() const { return layer_.hasCurrentResolution(); }
bool HeatmapChartPort::priceScaleCurrent() const { return layer_.priceScaleCurrent(); }
SpanLimits HeatmapChartPort::limits() const {
    return {layer_.minTimeSpanMs(), layer_.maxTimeSpanMs(), layer_.minPriceSpan(), layer_.maxPriceSpan()};
}
int64_t HeatmapChartPort::liveAnchorMs() const { return layer_.liveAnchorMs(); }
int64_t HeatmapChartPort::liveOpenEndMs() const { return layer_.liveOpenEndMs(); }
int64_t HeatmapChartPort::oldestAvailableMs() const { return layer_.oldestAvailableMs(); }
double HeatmapChartPort::recentMidPrice() const { return layer_.recentMidPrice(); }

int64_t HeatmapChartPort::predictTickUnits(const heatmap::gpu::ViewWindow& view) const {
    return layer_.predictTickUnits(view);
}
bool HeatmapChartPort::buildsTick(int64_t units, const heatmap::gpu::ViewWindow& view) const {
    return layer_.buildsTick(units, view);
}
int64_t HeatmapChartPort::autoTickState() const { return layer_.autoTickState(); }
heatmap::AutoTickParams HeatmapChartPort::autoTickParams() const {
    return {layer_.settings().minRowPx, layer_.settings().hysteresis};
}
bool HeatmapChartPort::setAutoPriceTick(std::optional<int64_t> units) {
    return layer_.setAutoPriceTick(units);
}
void HeatmapChartPort::setView(const heatmap::gpu::ViewWindow& drawn, bool priceKnown) {
    layer_.setView(drawn, priceKnown);
}

} // namespace chart
