#include "chart/core/ChartCamera.hpp"

#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/FrameContextBuilder.hpp"

#include <algorithm>
#include <cmath>

namespace chart {

ChartCamera::ChartCamera(const GridViewState& view, const ChartSession& session,
                         const IChartEnvironment& environment, const ITickOracle& ticks,
                         const PublishedFrame& published, const ZoomGlideState& glide)
    : view_(view), session_(session), environment_(environment), ticks_(ticks), published_(published), glide_(glide) {}

chart_raster::RasterInputs ChartCamera::rasterInputs(const FrameViewportSnapshot& viewport, double tick,
                                                             SurfaceFacts surface) const {
    chart_raster::RasterInputs in;
    if (!viewport.valid) return in;
    in.timeStart = viewport.timeStart;
    in.timeEnd = viewport.timeEnd;
    in.minPrice = viewport.minPrice;
    in.maxPrice = viewport.maxPrice;
    in.dragLogicalPx = viewport.dragging ? viewport.panVisualOffset : QPointF();
    in.tfMs = static_cast<double>(session_.timeframeMs);
    in.tick = tick;
    in.itemWidthLogical = surface.widthLogical;
    in.itemHeightLogical = surface.heightLogical;
    in.dpr = surface.dpr;
    in.anchorFracX = viewport.anchorFracX;
    in.anchorFracY = viewport.anchorFracY;
    return in;
}

// What a glide draws on its axis this frame (without a drag): its end while holding.
chart_raster::RasterCamera glidePart(const chart_raster::RasterCamera& from, const chart_raster::RasterCamera& end,
                                     double anchorTime, double anchorPrice, double progress, bool holding) {
  return holding ? end : chart_raster::glideRaster(from, end, anchorTime, anchorPrice, progress);
}

Cameras camerasFor(const CameraInputs& camera) {
  const auto& in = camera.raster;
  const auto previous = camera.previous;
  const auto& glide_ = camera.glide;
  Cameras c;
  c.rest = chart_raster::computeRaster(in, previous);
  c.drawn = c.rest;
  if (!c.rest.valid) return c;
  const bool gestureTime = glide_.gesture && glide_.gestureTime, gesturePrice = glide_.gesture && glide_.gesturePrice;
  const bool timeMoves = gestureTime || glide_.time.active, priceMoves = gesturePrice || glide_.price.active;
  if (!timeMoves && !priceMoves) return c;
  // Each axis on its own: a gesture's continuous camera, a glide, or the rest camera.
  // A drag moves a glide's ends by the same whole device pixels as the picture (1:1).
  const double dpr = c.rest.dpr;
  const double dx = std::isfinite(in.dragLogicalPx.x()) ? double(std::llround(in.dragLogicalPx.x() * dpr)) : 0.0;
  const double dy = std::isfinite(in.dragLogicalPx.y()) ? double(std::llround(in.dragLogicalPx.y() * dpr)) : 0.0;
  const chart_raster::RasterCamera continuous =
      gestureTime || gesturePrice ? chart_raster::continuousRaster(in) : chart_raster::RasterCamera{};
  double timeLo = c.rest.drawnStartMs, timeHi = c.rest.drawnEndMs;
  double priceLo = c.rest.drawnMinPrice, priceHi = c.rest.drawnMaxPrice;
  auto axis = [&](const AxisGlide& g, bool time, bool gesture) {
    if (gesture) return continuous;
    if (!g.active) return c.rest;
    const auto from = chart_raster::shiftedRaster(g.from, dx, dy), end = chart_raster::shiftedRaster(g.end, dx, dy);
    // Bin the glide's whole extent (its start and its end) once, at its start.
    if (time) {
      timeLo = std::min({timeLo, from.drawnStartMs, end.drawnStartMs});
      timeHi = std::max({timeHi, from.drawnEndMs, end.drawnEndMs});
    } else {
      priceLo = std::min({priceLo, from.drawnMinPrice, end.drawnMinPrice});
      priceHi = std::max({priceHi, from.drawnMaxPrice, end.drawnMaxPrice});
    }
    return glidePart(from, end, g.anchorTime, g.anchorPrice, g.progress, g.holding);
  };
  const auto timeCam = axis(glide_.time, true, gestureTime);
  const auto priceCam = axis(glide_.price, false, gesturePrice);
  c.drawn = chart_raster::composeAxes(timeCam, timeMoves, priceCam, priceMoves, c.rest);
  if (c.drawn.free) {
    c.binView.emplace();
    c.binView->timeLo = std::min(timeLo, c.drawn.drawnStartMs);
    c.binView->timeHi = std::max(timeHi, c.drawn.drawnEndMs);
    c.binView->priceLo = std::min(priceLo, c.drawn.drawnMinPrice);
    c.binView->priceHi = std::max(priceHi, c.drawn.drawnMaxPrice);
  }
  return c;
}

chart_raster::RasterCamera ChartCamera::rasterCameraNow(bool includeDrag) const {
  if (!environment_.attached()) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(&view_);
  if (!includeDrag) viewport.dragging = false;
  const auto surface = environment_.surface();
  const auto in = rasterInputs(viewport, environment_.drawnTick(), surface);
  const chart_raster::RasterStep previous = published_.step();
  return camerasFor({in, previous, glide_}).drawn;
}

chart_raster::RasterCamera ChartCamera::restCameraNow(bool includeDrag) const {
  if (!environment_.attached()) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(&view_);
  if (!includeDrag) viewport.dragging = false;
  const auto surface = environment_.surface();
  const auto in = rasterInputs(viewport, environment_.drawnTick(), surface);
  const chart_raster::RasterStep previous = published_.step();
  return chart_raster::computeRaster(in, previous);
}

chart_raster::RasterCamera ChartCamera::intendedEndNow() const {
  if (!environment_.attached()) return {};
  auto viewport = FrameContextBuilder::viewportSnapshot(&view_);
  viewport.dragging = false;
  // The tick the frame chooses for the stored view (the chooser's own rule and state),
  // before it is committed.
  double tick = environment_.drawnTick();
  if (tick > 0 && viewport.valid) {
    const int64_t units = ticks_.predictTickUnits(
        {double(viewport.timeStart), double(viewport.timeEnd), viewport.minPrice, viewport.maxPrice});
    if (units > 0) tick = heatmap::fromUnits(units, environment_.priceScale());
  }
  const auto surface = environment_.surface();
  const auto in = rasterInputs(viewport, tick, surface);
  const chart_raster::RasterStep previous = published_.step();
  return chart_raster::computeRaster(in, previous);
}

chart_raster::TickAt ChartCamera::tickPredictor(qint64 start, qint64 end, double anchorPrice,
                                                       double fracY) const {
  // The window a price span is drawn with: about the anchor (Auto judges the rows in
  // view, so the window matters, not only its span).
  return [this, start, end, anchorPrice, fracY](double span) -> double {
    if (!environment_.attached() || !(span > 0)) return 0.0;
    const double hi = anchorPrice + fracY * span;
    const heatmap::gpu::ViewWindow view{double(start), double(end), hi - span, hi};
    const int64_t units = ticks_.predictTickUnits(view);
    return units > 0 ? heatmap::fromUnits(units, environment_.priceScale()) : 0.0;
  };
}

bool ChartCamera::panShift(QPointF drag, qint64& timeShiftMs, double& priceShift) const {
  if (!environment_.attached()) return false;
  auto viewport = FrameContextBuilder::viewportSnapshot(&view_);
  viewport.dragging = false; // the committed view; panShift applies the drag
  const auto surface = environment_.surface();
  chart_raster::RasterStep previous = published_.step();
  // An axis that glides is committed against its fixed end (its tick and whole pixels,
  // the predicted tick before it commits): the committed view then reproduces the end
  // translated by the dragged pixels (endPanAt moves the end by them). An axis at rest
  // uses the rest camera.
  double tick = environment_.drawnTick();
  if (glide_.price.active && glide_.price.end.valid) {
    tick = glide_.price.end.tick;
    previous.rowPx = glide_.price.end.rowPx;
  }
  if (glide_.time.active && glide_.time.end.valid) previous.colPx = glide_.time.end.colPx;
  const auto in = rasterInputs(viewport, tick, surface);
  int64_t time = 0;
  if (!chart_raster::panShift(in, previous, drag, time, priceShift)) return false;
  timeShiftMs = time;
  return true;
}

std::pair<double, double> ChartCamera::drawnTimeWindow(qint64 start, qint64 end,
                                                               std::optional<double> anchorFracX) const {
  if (!environment_.attached() || !(end > start)) return {double(start), double(end)};
  auto viewport = FrameContextBuilder::viewportSnapshot(&view_);
  viewport.valid = true;
  viewport.timeStart = start;
  viewport.timeEnd = end;
  viewport.dragging = false; // a drag is already in [start, end] (displayedTimeWindow)
  if (!(viewport.maxPrice > viewport.minPrice)) { // the time window does not depend on price
    viewport.minPrice = 0.0;
    viewport.maxPrice = 1.0;
  }
  if (anchorFracX) viewport.anchorFracX = *anchorFracX;
  const auto surface = environment_.surface();
  const chart_raster::RasterStep previous = published_.step();
  const auto cam = chart_raster::computeRaster(rasterInputs(viewport, environment_.drawnTick(), surface),
                                               previous);
  if (!cam.valid) return {double(start), double(end)};
  return {cam.drawnStartMs, cam.drawnEndMs};
}

} // namespace chart
