#include "chart/shell/HeatmapChartPort.hpp"

#include "render/heatmap/HeatmapGpuLayer.hpp"

namespace chart {

double HeatmapChartPort::drawnTick() const { return layer_ ? layer_->tickPrice() : 0.0; }
double HeatmapChartPort::priceScale() const { return layer_ ? layer_->priceScale() : 0.0; }
bool HeatmapChartPort::manualTick() const { return layer_ && layer_->manualMode(); }
bool HeatmapChartPort::hasCurrentResolution() const { return layer_ && layer_->hasCurrentResolution(); }
bool HeatmapChartPort::priceScaleCurrent() const { return layer_ && layer_->priceScaleCurrent(); }
SpanLimits HeatmapChartPort::limits() const {
    if (!layer_) return {};
    return {layer_->minTimeSpanMs(), layer_->maxTimeSpanMs(), layer_->minPriceSpan(), layer_->maxPriceSpan()};
}
int64_t HeatmapChartPort::liveAnchorMs() const { return layer_ ? layer_->liveAnchorMs() : 0; }
int64_t HeatmapChartPort::liveOpenEndMs() const { return layer_ ? layer_->liveOpenEndMs() : 0; }
int64_t HeatmapChartPort::oldestAvailableMs() const { return layer_ ? layer_->oldestAvailableMs() : 0; }
double HeatmapChartPort::recentMidPrice() const { return layer_ ? layer_->recentMidPrice() : 0.0; }

int64_t HeatmapChartPort::predictTickUnits(const heatmap::gpu::ViewWindow& view) const {
    return layer_ ? layer_->predictTickUnits(view) : 0;
}
bool HeatmapChartPort::buildsTick(int64_t units, const heatmap::gpu::ViewWindow& view) const {
    return layer_ && layer_->buildsTick(units, view);
}
int64_t HeatmapChartPort::autoTickState() const { return layer_ ? layer_->autoTickState() : 0; }
heatmap::AutoTickParams HeatmapChartPort::autoTickParams() const {
    return layer_ ? heatmap::AutoTickParams{layer_->settings().minRowPx, layer_->settings().hysteresis}
                  : heatmap::AutoTickParams{};
}
bool HeatmapChartPort::setAutoPriceTick(std::optional<int64_t> units) {
    return layer_ && layer_->setAutoPriceTick(units);
}
void HeatmapChartPort::setView(const heatmap::gpu::ViewWindow& drawn, bool priceKnown) {
    if (layer_) layer_->setView(drawn, priceKnown);
}

void HeatmapChartPort::chooseTickForView(const heatmap::gpu::ViewWindow& view) {
    if (layer_) layer_->chooseTickForView(view);
}
double HeatmapChartPort::tickPrice() const { return layer_ ? layer_->tickPrice() : 0.0; }
void HeatmapChartPort::noteRaster(int rowPx, int colPx, double rowPxContinuous) {
    if (layer_) layer_->noteRaster(rowPx, colPx, rowPxContinuous);
}

} // namespace chart
