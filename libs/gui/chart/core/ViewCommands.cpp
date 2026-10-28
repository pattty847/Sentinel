#include "chart/core/ViewCommands.hpp"
#include "chart/core/ChartViewController.hpp"
#include "SentinelLogging.hpp"
#include <cmath>
#include <tuple>

namespace chart {
ViewCommands::ViewCommands(ChartViewController& controller, IChartEnvironment& environment, ITickOracle& ticks)
    : controller_(controller), environment_(environment), ticks_(ticks) {}

void ViewCommands::setAutoPriceScale(bool enabled) {
  if (!controller_.viewPtr()) return;
  if (!enabled) {
    controller_.viewPtr()->setAutoPriceScale(false); // the price range stays where it is
    return;
  }
  // On = the price-axis double-click: fit now (candles, else the live price); with
  // nothing to fit yet it is on and the next change in view fits.
  if (!fitView(false, true)) controller_.viewPtr()->setAutoPriceScale(true);
}

void ViewCommands::releasePrice(const char* reason) {
  if (!controller_.viewPtr()) return;
  controller_.viewPtr()->setAutoPriceScale(false);
  controller_.midLock().release(reason);
}

bool ViewCommands::fitView(bool time, bool price) {
  controller_.zoomGlide().land();
  if (fitUnavailable(time, price)) {
    sLog_Render("view fit skipped: time=" << time << " price=" << price);
    return false;
  }
  FitWindow bounds{controller_.viewPtr()->getVisibleTimeStart(), controller_.viewPtr()->getVisibleTimeEnd(),
                   controller_.viewPtr()->getMinPrice(), controller_.viewPtr()->getMaxPrice()};
  auto& [start, end, lo, hi] = bounds;
  const bool ok = fitAxes(time, price, bounds);
  sLog_Render("view fit time=" << time << " price=" << price << " applied=" << ok << " time=[" << start << ".."
              << end << "] price=[" << lo << ".." << hi << "]");
  if (!ok) return false;
  if (price) {
    controller_.flags().markPriceKnown();
    controller_.viewPtr()->setAutoPriceScale(true); // a price fit is the auto price scale
  }
  // Raster anchor of the fitted axes: the view end (live edge) and the price centre.
  const auto anchor = controller_.viewPtr()->rasterAnchor();
  controller_.viewPtr()->setRasterAnchor(time ? 1.0 : anchor.fracX, price ? 0.5 : anchor.fracY);
  controller_.viewPtr()->setViewport(start, end, lo, hi); // one change; follow-live is kept
  controller_.syncGpuView();
  emit updateRequested();
  return true;
}

bool ViewCommands::fitUnavailable(bool time, bool price) const {
  return !environment_.attached() || !controller_.viewPtr() || !controller_.viewPtr()->isTimeWindowValid() || (!time && !price) ||
      controller_.viewPtr()->isDragging();
}

bool ViewCommands::fitAxes(bool time, bool price, FitWindow& bounds) {
  auto& [start, end, lo, hi] = bounds;
  // All or nothing: every requested part must fit, else nothing changes (a failed fit
  // leaves the view, follow-live and the auto price scale as they were).
  bool ok = true;
  if (time) {
    if (const auto window = controller_.priceFitter().gpuFitTimeWindow()) std::tie(start, end) = *window;
    else ok = false;
  }
  if (price && ok) {
    // Candles, else the live price (setViewport's auto fit takes the candles first),
    // over what the fitted window draws (a time fit anchors the view end).
    if (const auto window = controller_.priceFitter().gpuFitPriceWindow(start, end, false, time ? std::optional<double>(1.0) : std::nullopt))
      std::tie(lo, hi) = *window;
    else ok = false;
  }
  return ok;
}

QString ViewCommands::applyViewportRequest(const ViewportRequest& request) {
  if (!controller_.viewPtr()) return QStringLiteral("viewport_unavailable");
  controller_.zoomGlide().land();
  if (!request.fit.isEmpty()) {
    const bool time = request.fit != "price", price = request.fit != "time";
    const bool ok = request.fit == "default" ? resetView() : fitView(time, price);
    return ok ? QString() : QStringLiteral("fit_unavailable");
  }
  const qint64 start0 = controller_.viewPtr()->getVisibleTimeStart(), end0 = controller_.viewPtr()->getVisibleTimeEnd();
  const bool follow = resolveFlags(request);
  commit(resolveWindow(request, start0, end0, follow));
  return {};
}

bool ViewCommands::resolveFlags(const ViewportRequest& request) {
  const bool explicitPrice = request.priceMin && request.priceMax;
  const bool explicitTime = request.startMs && request.endMs;
  // The flags first (no viewport change of their own), then one commit.
  if (explicitPrice) {
    releasePrice("explicit price bounds"); // the user's price, equal or not
    controller_.priceFitter().priceCarry().reset();
  } else {
    // autoScale:true releases the lock; lockMid:true turns the auto price scale off.
    if (request.autoScale) controller_.viewPtr()->setAutoPriceScale(*request.autoScale);
    if (request.lockMid) controller_.midLock().setEnabled(*request.lockMid);
  }
  const bool follow = (explicitTime || explicitPrice) ? false : request.followLive.value_or(controller_.viewPtr()->isAutoScrollEnabled());
  controller_.viewPtr()->enableAutoScroll(follow);
  return follow;
}

ViewCommands::RequestWindow ViewCommands::resolveWindow(const ViewportRequest& request, qint64 start0, qint64 end0, bool follow) {
  const bool explicitPrice = request.priceMin && request.priceMax;
  const bool explicitTime = request.startMs && request.endMs;
  qint64 start = request.startMs.value_or(start0), end = request.endMs.value_or(end0);
  if (follow && !explicitTime) {
    // Follow-live on: the live edge one padding inside, span kept (returnGpuToLive).
    const qint64 span = end - start;
    if (const qint64 liveEnd = controller_.priceFitter().gpuLiveEndMs(span); liveEnd > 0) {
      end = liveEnd;
      start = liveEnd - span;
    }
  }
  double lo = request.priceMin.value_or(controller_.viewPtr()->getMinPrice());
  double hi = request.priceMax.value_or(controller_.viewPtr()->getMaxPrice());
  if (explicitPrice) {
    controller_.flags().markPriceKnown();
  } else if (request.autoScale.value_or(false)) {
    // On: the price-axis double-click (candles; with none, the live price centred), over
    // what the requested window draws with the anchor it gets below.
    const double fracX = follow ? 1.0 : (start != start0 || end != end0) ? 0.5 : controller_.viewPtr()->rasterAnchor().fracX;
    if (const auto window = controller_.priceFitter().gpuFitPriceWindow(start, end, false, fracX)) {
      std::tie(lo, hi) = *window;
      controller_.flags().markPriceKnown();
    }
  }
  return {start0, end0, start, end, lo, hi, follow};
}

void ViewCommands::commit(const RequestWindow& window) {
  const auto& [start0, end0, start, end, lo, hi, follow] = window;
  sLog_Render("viewport request: time=[" << start << ".." << end << "] price=[" << lo << ".." << hi
              << "] follow=" << follow << " autoPriceScale=" << controller_.viewPtr()->autoPriceScale());
  // Raster anchor: the centre of an explicitly moved axis (following live keeps the
  // live edge); an unchanged axis keeps its anchor (no re-snap).
  const auto anchor = controller_.viewPtr()->rasterAnchor();
  const bool timeMoved = start != start0 || end != end0;
  const bool priceMoved = lo != controller_.viewPtr()->getMinPrice() || hi != controller_.viewPtr()->getMaxPrice();
  controller_.viewPtr()->setRasterAnchor(follow ? 1.0 : timeMoved ? 0.5 : anchor.fracX, priceMoved ? 0.5 : anchor.fracY);
  controller_.viewPtr()->setViewport(start, end, lo, hi); // one change (the auto fit applies inside)
  controller_.syncGpuView();
  if (follow) emit liveRenderTick();
  emit updateRequested();
}

bool ViewCommands::resetView() {
  controller_.zoomGlide().land();
  if (!environment_.attached() || !controller_.viewPtr() || !controller_.viewPtr()->isTimeWindowValid() ||
      controller_.viewPtr()->isDragging())
    return false;
  int64_t span = controller_.livePlacement().gpuInitialSpanMs(environment_.surface().widthLogical);
  if (const double maxTime = controller_.viewPtr()->maxTimeSpanMs(); maxTime > 0)
    span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))));
  const int64_t end = controller_.priceFitter().gpuLiveEndMs(span);
  if (end <= 0) {
    sLog_Render("view reset skipped: no live anchor yet");
    return false;
  }
  controller_.viewPtr()->setAutoPriceScale(true);
  controller_.viewPtr()->enableAutoScroll(true); // the view state's flag only: the viewport moves once, below
  double lo = controller_.viewPtr()->getMinPrice(), hi = controller_.viewPtr()->getMaxPrice();
  if (const auto window = controller_.priceFitter().gpuFitPriceWindow(end - span, end, false, 1.0)) {
    std::tie(lo, hi) = *window;
    controller_.flags().markPriceKnown();
  }
  sLog_Render("view reset to default: time=[" << end - span << ".." << end << "] price=[" << lo << ".." << hi << "]");
  controller_.viewPtr()->setRasterAnchor(1.0, 0.5);
  controller_.setGpuViewportSelf(end - span, end, lo, hi);
  controller_.flags().pristine = true; // the default span follows the chart's width again
  controller_.syncGpuView();
  emit liveRenderTick();
  emit updateRequested();
  return true;
}

void ViewCommands::setViewport(qint64 timeStart, qint64 timeEnd,
                                      double priceMin, double priceMax) {
  controller_.zoomGlide().land();
  if (priceMax > priceMin) {
    // An explicit viewport (Agent API, QML) is a real price window: no book-top
    // seed replaces it.
    controller_.flags().markPriceKnown();
    controller_.priceFitter().priceCarry().reset();
  }
  if (controller_.viewPtr() && (priceMin != controller_.viewPtr()->getMinPrice() ||
                      priceMax != controller_.viewPtr()->getMaxPrice())) {
    // An explicit price range turns follow-live, the auto price scale and the lock to
    // mid off (before the viewport moves: the auto fit would replace it).
    releasePrice("explicit viewport");
    controller_.viewPtr()->enableAutoScroll(false);
  }
  anchorExplicitViewport(timeStart, timeEnd, priceMin, priceMax);
  onViewChanged(timeStart, timeEnd, priceMin, priceMax);
  controller_.syncGpuView(); // also when the values were unchanged (price now known)
}

void ViewCommands::anchorExplicitViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax) {
  if (controller_.viewPtr()) {
    // Raster anchor: the centre of a moved axis; an unchanged axis keeps its anchor.
    const auto anchor = controller_.viewPtr()->rasterAnchor();
    const bool timeMoved =
        timeStart != controller_.viewPtr()->getVisibleTimeStart() || timeEnd != controller_.viewPtr()->getVisibleTimeEnd();
    const bool priceMoved = priceMin != controller_.viewPtr()->getMinPrice() || priceMax != controller_.viewPtr()->getMaxPrice();
    controller_.viewPtr()->setRasterAnchor(timeMoved && !controller_.viewPtr()->isAutoScrollEnabled() ? 0.5 : anchor.fracX,
                                 priceMoved ? 0.5 : anchor.fracY);
  }
}

void ViewCommands::resetZoom() {
  if (controller_.viewPtr()) {
    controller_.viewPtr()->resetZoom();
    emit updateRequested();
  }
}

void ViewCommands::beginPanAt(double x, double y) {
  if (!controller_.viewPtr() || !environment_.visible()) {
    return;
  }
  if (controller_.viewPtr()->isAutoScrollEnabled()) {
    controller_.viewPtr()->enableAutoScroll(false);
    sLog_Render("auto-scroll enabled=false reason=pan");
  }
  controller_.viewPtr()->handlePanStart(QPointF(x, y));
  emit updateRequested();
}

void ViewCommands::updatePanAt(double x, double y) {
  if (!controller_.viewPtr() || !environment_.visible()) {
    return;
  }
  controller_.viewPtr()->handlePanMove(QPointF(x, y));
  emit updateRequested();
}

void ViewCommands::endPanAt() {
  if (!controller_.viewPtr()) {
    return;
  }
  // A zoom glide in flight: its start moves with the committed whole pixels too.
  const bool dragging = controller_.viewPtr()->isDragging();
  const QPointF drag = controller_.viewPtr()->getPanVisualOffset();
  const double dpr = environment_.surface().dpr;
  controller_.viewPtr()->handlePanEnd(true);
  if (dragging) {
    const double dx = double(std::llround(drag.x() * dpr)), dy = double(std::llround(drag.y() * dpr));
    controller_.zoomGlide().shiftBoth(dx, dy);
  }
  emit updateRequested();
}

void ViewCommands::panLeft() {
  if (controller_.viewPtr()) {
    controller_.viewPtr()->panLeft();
    emit updateRequested();
  }
}

void ViewCommands::panRight() {
  if (controller_.viewPtr()) {
    controller_.viewPtr()->panRight();
    emit updateRequested();
  }
}

void ViewCommands::panUp() {
  if (controller_.viewPtr()) {
    controller_.midLock().release("keyboard price pan"); // the auto price scale ignores it
    controller_.viewPtr()->panUp();
    emit updateRequested();
  }
}

void ViewCommands::panDown() {
  if (controller_.viewPtr()) {
    controller_.midLock().release("keyboard price pan");
    controller_.viewPtr()->panDown();
    emit updateRequested();
  }
}

void ViewCommands::onViewChanged(qint64 startTimeMs, qint64 endTimeMs,
                                        double minPrice, double maxPrice) {
  if (controller_.viewPtr()) {
    controller_.viewPtr()->setViewport(startTimeMs, endTimeMs, minPrice, maxPrice);
  }

  emit updateRequested();

  sLog_Probe("viewport.view",
             "t=[" << startTimeMs << ".." << endTimeMs << "]"
             << " price=[" << minPrice << ".." << maxPrice << "]");
}

void ViewCommands::onViewportChanged() {
  if (!controller_.viewPtr()) return;
  emit updateRequested();
  if (!controller_.flags().selfViewport) controller_.flags().pristine = false; // someone else moved the view
  // The controller plans from the committed view.
  controller_.syncGpuView();
}

void ViewCommands::onHeatmapSnapshot() {
        // No book top yet (or a symbol switch): centre on the decoded data instead.
        if ((!controller_.flags().priceKnown || controller_.flags().reseedPrice) && controller_.viewPtr()->isTimeWindowValid()) {
            if (const double mid = environment_.recentMidPrice(); mid > 0) controller_.livePlacement().seedGpuViewport(mid, mid);
        }
        emit updateRequested();
}

void ViewCommands::onHeatmapLive() {
  livePriceFitRevision_ = controller_.priceFitter().autoPriceFitRevision();
  controller_.livePlacement().followGpuLive();
  emit liveRenderTick();
  emit updateRequested();
}

void ViewCommands::refitAfterHeatmapLive() {
  if (controller_.priceFitter().autoPriceFitRevision() == livePriceFitRevision_)
    controller_.priceFitter().refitAutoPrice();
}

void ViewCommands::onHeatmapTick() {
        controller_.applyGpuLimits();
        controller_.priceFitter().refitAutoPrice(); // the fit's min/max price spans follow the tick
        controller_.syncGpuView();    // the drawn rows (and so the drawn price window) follow it too
        // Before the frame that draws the new tick: the axis models recalculate on
        // heatmapTickSizeChanged, the sibling overlays (candles, paper, algo) redraw on
        // viewportChanged; all of them sync with the new raster camera in that frame.
        emit heatmapTickSizeChanged();
        emit viewportChanged();
        emit updateRequested();
}

void ViewCommands::onHeatmapLimits() {
        controller_.applyGpuLimits();
        controller_.priceCarry().limitsChanged();
        controller_.priceFitter().refitAutoPrice(); // e.g. a new symbol's tick: its min/max price spans
}

void ViewCommands::onAutoPriceScaleChanged() {
    sLog_Render("auto price scale=" << controller_.viewPtr()->autoPriceScale());
    if (controller_.viewPtr()->autoPriceScale()) controller_.priceFitter().priceCarry().reset();
    emit autoPriceScaleChanged();
}

void ViewCommands::clearAutoPriceTick() {
    if (!controller_.viewPtr()->autoPriceScale() && ticks_.setAutoPriceTick(std::nullopt)) emit updateRequested();
}

} // namespace chart
