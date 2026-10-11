#include "chart/core/ZoomGlide.hpp"

#include "SentinelLogging.hpp"
#include "chart/core/ChartCamera.hpp"

#include <algorithm>

namespace chart {

ZoomGlide::ZoomGlide(ZoomGlideState& state, const ChartCamera& camera)
    : state_(state), camera_(camera) {}

void ZoomGlide::endZoomGlide(bool time, bool price) {
  bool ended = false;
  if (time && state_.time.active) ended = true, state_.time.active = false;
  if (price && state_.price.active) ended = true, state_.price.active = false;
  if (ended) emit updateRequested();
}

void ZoomGlide::startAxisGlides(const chart_raster::RasterCamera& drawn,
                                 const chart_raster::RasterCamera& restBefore, double anchorTime,
                                 double anchorPrice, bool forceTime, bool forcePrice) {
  const auto end = camera_.intendedEndNow();
  if (!end.valid || !drawn.valid) return;
  const qint64 now = now_();
  bool started = false;
  auto start = [&](AxisGlide& g, bool time, bool force) {
    // The end this axis had: its glide's, or the rest camera it was drawn at.
    const auto& before = g.active ? g.end : restBefore;
    const bool same = time ? chart_raster::sameTimeAxis(end, before) : chart_raster::samePriceAxis(end, before);
    if (same && !force) return; // unchanged: a running glide keeps going, a rest axis stays
    g.active = true;
    g.holding = false;
    g.from = drawn;
    g.end = end;
    g.anchorTime = anchorTime;
    g.anchorPrice = anchorPrice;
    g.beganMs = g.startMs = now;
    g.deadlineMs = now + chart_raster::kZoomGlideMs;
    g.progress = 0.0;
    started = true;
  };
  start(state_.time, true, forceTime);
  start(state_.price, false, forcePrice);
  if (state_.time.active) state_.colPx = state_.time.end.colPx;
  if (state_.price.active) state_.row = {state_.price.end.tick, state_.price.end.rowPx};
  if (!started) return;
  sLog_Probe("zoom.glide", "start time=" << state_.time.active << " colPx=" << drawn.colPxF << "->" << end.colPx
             << " price=" << state_.price.active << " rows=" << drawn.pxPerPrice() << "->" << end.rowPx << "@"
             << end.tick);
  emit rasterChanged();
  emit viewportChanged();
  emit polishRequested(); // the next frame's updatePolish advances it
  emit updateRequested();
}

void ZoomGlide::advanceAxisGlide(AxisGlide& g, bool time, const chart_raster::RasterCamera& rest,
                                  const chart_raster::RasterCamera& intended, qint64 now) {
  if (!g.active) return;
  const qint64 cap = g.beganMs + 2 * chart_raster::kZoomGlideMs;
  // The end is fixed at the click. A change of it that the user did not ask for (a
  // refit, another device pixel ratio or size, a tick that differs from the
  // predicted one) continues from what the last frame drew: no jump, no reverse
  // movement. A drag, a pan commit and a follow-live step move both ends instead.
  const bool same = time ? chart_raster::sameTimeAxis(intended, g.end) : chart_raster::samePriceAxis(intended, g.end);
  if (intended.valid && !same) {
    g.from = chart::glidePart(g.from, g.end, g.anchorTime, g.anchorPrice, g.progress, g.holding);
    g.end = intended;
    g.holding = false;
    g.deadlineMs = std::min(cap, std::max(g.deadlineMs, now + chart_raster::kZoomGlideMs / 2));
    g.startMs = now;
    g.progress = 0.0;
    ++state_.rebases;
    if (time) state_.colPx = g.end.colPx;
    else state_.row = {g.end.tick, g.end.rowPx};
    sLog_Probe("zoom.glide", "rebased " << (time ? "time" : "price") << " to colPx=" << g.end.colPx << " rowPx="
               << g.end.rowPx << "@" << g.end.tick << " dpr=" << g.end.dpr << " leftMs=" << g.deadlineMs - now);
  }
  // At the cap the frame lands on the current rest camera (as at rest).
  if (now >= cap) {
    g.active = false;
    g.progress = 1.0;
    sLog_Probe("zoom.glide", "landed at the cap " << (time ? "time" : "price"));
    return;
  }
  const double t = double(now - g.startMs) / double(std::max<qint64>(1, g.deadlineMs - g.startMs));
  if (t < 1.0) {
    g.progress = chart_raster::easeZoom(t);
    return;
  }
  g.progress = 1.0;
  // The end is reached. It lands when the frame's rest camera is the end (the landing
  // frame is the rest frame); until then (the end's tick not committed yet) it holds
  // the end, drawn with coverage.
  const bool landed = time ? chart_raster::sameTimeAxis(rest, g.end) : chart_raster::samePriceAxis(rest, g.end);
  g.holding = !landed;
  if (landed) {
    g.active = false;
    sLog_Probe("zoom.glide", "landed " << (time ? "time" : "price"));
  }
}

void ZoomGlide::advanceZoomGlide(qint64 now) {
  if (!gliding()) return;
  const auto rest = camera_.restCameraNow(false), intended = camera_.intendedEndNow();
  advanceAxisGlide(state_.time, true, rest, intended, now);
  advanceAxisGlide(state_.price, false, rest, intended, now);
  // This frame's camera for every layer: the axis models recalculate, the sibling
  // overlays (candles, paper, algo) redraw in this synchronization.
  emit rasterChanged();
  emit viewportChanged();
  emit updateRequested();
}

void ZoomGlide::shiftTime(qint64 shift) {
  if (state_.time.active) {
    auto& g = state_.time;
    g.from = chart_raster::shiftedRaster(g.from, -double(shift) * g.from.pxPerMs(), 0);
    g.end = chart_raster::shiftedRaster(g.end, -double(shift) * g.end.pxPerMs(), 0);
    g.anchorTime += double(shift);
  }
}

void ZoomGlide::shiftBoth(double dx, double dy) {
  // A glide in flight: the release commits the dragged whole pixels, so both of its
  // ends move by them too (the picture stays where the drag left it).
  for (AxisGlide* g : {&state_.time, &state_.price})
    if (g->active) {
      g->from = chart_raster::shiftedRaster(g->from, dx, dy);
      g->end = chart_raster::shiftedRaster(g->end, dx, dy);
    }
}

} // namespace chart
