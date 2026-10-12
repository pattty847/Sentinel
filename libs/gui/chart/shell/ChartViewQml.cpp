#include "ChartViewQml.hpp"
namespace chart {
void ChartViewQml::zoomClicks(int clicks, double x, double y, bool time, bool price) {
  controller_->zoom().zoomClicks(clicks, x, y, time, price);
}

void ChartViewQml::zoomContinuous(double factor, double x, double y, bool time, bool price) {
  controller_->zoom().zoomContinuous(factor, x, y, time, price);
}

void ChartViewQml::endZoomGesture() {
  controller_->zoom().endZoomGesture();
}

void ChartViewQml::zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x) {
  controller_->zoom().zoomTimeWheel(angleDelta, pixelDelta, phase, x);
}

void ChartViewQml::zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y) {
  controller_->zoom().zoomPriceWheel(angleDelta, pixelDelta, phase, y);
}

void ChartViewQml::zoomTimeDrag(double delta, double x) {
  controller_->zoom().zoomTimeDrag(delta, x);
}

void ChartViewQml::zoomPriceDrag(double delta, double y) {
  controller_->zoom().zoomPriceDrag(delta, y);
}

void ChartViewQml::zoomTimeClicks(int clicks, double x) {
  controller_->zoom().zoomTimeClicks(clicks, x);
}

void ChartViewQml::zoomPriceClicks(int clicks, double y) {
  controller_->zoom().zoomPriceClicks(clicks, y);
}

void ChartViewQml::setTimeframe(int timeframe_ms) {
  controller_->setTimeframe(timeframe_ms);
}

void ChartViewQml::enableAutoScroll(bool enabled) {
  controller_->livePlacement().enableAutoScroll(enabled);
}

double ChartViewQml::heatmapTickSize() const {
  return environment_ ? environment_->drawnTick() : 0.0;
}

void ChartViewQml::setAutoPriceScale(bool enabled) { controller_->commands().setAutoPriceScale(enabled); }

bool ChartViewQml::fitView(bool time, bool price) { return controller_->commands().fitView(time, price); }

bool ChartViewQml::resetView() { return controller_->commands().resetView(); }

qint64 ChartViewQml::getVisibleTimeStart() const { return getViewState() ? getViewState()->getVisibleTimeStart() : 0; }

qint64 ChartViewQml::getVisibleTimeEnd() const { return getViewState() ? getViewState()->getVisibleTimeEnd() : 0; }

double ChartViewQml::getMinPrice() const { return getViewState() ? getViewState()->getMinPrice() : 0.0; }

double ChartViewQml::getMaxPrice() const { return getViewState() ? getViewState()->getMaxPrice() : 0.0; }

QPointF ChartViewQml::getPanVisualOffset() const { return getViewState() ? getViewState()->getPanVisualOffset() : QPointF(0, 0); }

void ChartViewQml::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) { controller_->commands().setViewport(timeStart, timeEnd, priceMin, priceMax); }

void ChartViewQml::zoomIn() { zoomClicks(1, width() / 2, height() / 2); }

void ChartViewQml::zoomOut() { zoomClicks(-1, width() / 2, height() / 2); }

void ChartViewQml::resetZoom() { controller_->commands().resetZoom(); }

void ChartViewQml::beginPanAt(double x, double y) { controller_->commands().beginPanAt(x, y); }

void ChartViewQml::updatePanAt(double x, double y) { controller_->commands().updatePanAt(x, y); }

void ChartViewQml::endPanAt() { controller_->commands().endPanAt(); }

void ChartViewQml::panLeft() { controller_->commands().panLeft(); }

void ChartViewQml::panRight() { controller_->commands().panRight(); }

void ChartViewQml::panUp() { controller_->commands().panUp(); }

void ChartViewQml::panDown() { controller_->commands().panDown(); }
} // namespace chart
