#include "chart/core/ZoomController.hpp"
#include "chart/core/ChartViewController.hpp"
#include "SentinelLogging.hpp"
#include "chart/core/PolicyTimer.hpp"
#include <algorithm>
#include <cmath>

namespace chart {
namespace {
// The continuous zoom of one event (an axis drag step, a trackpad scroll delta in
// angle units), as the wheel always scaled it: 1 + clamp(delta * 0.0005, +-0.4)
// (> 1 zooms in).
constexpr double kContinuousZoomPerUnit = 0.0005, kContinuousZoomMaxStep = 0.4;
double continuousFactor(double delta) {
  return 1.0 + std::clamp(delta * kContinuousZoomPerUnit, -kContinuousZoomMaxStep, kContinuousZoomMaxStep);
}
using SettleTimer = PolicyTimer<IZoomTimer>;
} // namespace

ZoomController::ZoomController(ChartViewController& controller, IChartEnvironment& environment)
    : controller_(controller), environment_(environment),
      timerFactory_([](std::function<void()> timeout) { return std::make_unique<SettleTimer>(std::move(timeout)); }) {}

void ZoomController::setTimerFactoryForTest(TimerFactory factory) {
  settleTimer_.reset();
  timerFactory_ = std::move(factory);
}

void ZoomController::stop() { if (settleTimer_) settleTimer_->stop(); }
bool ZoomController::gestureSettlePending() const { return settleTimer_ && settleTimer_->isActive(); }

GridViewState* ZoomController::getViewState() const { return controller_.viewPtr(); }
double ZoomController::width() const { return environment_.surface().widthLogical; }
double ZoomController::height() const { return environment_.surface().heightLogical; }

std::pair<double, double> ZoomController::zoomFrac(double x, double y) const {
  const double fracX = std::clamp(width() > 0 ? x / width() : 0.5, 0.0, 1.0);
  const double fracY = std::clamp(height() > 0 ? y / height() : 0.5, 0.0, 1.0);
  return {fracX, fracY};
}

std::pair<int, int> ZoomController::columnLimits(double W, double tf) const {
  const double maxTime = getViewState()->maxTimeSpanMs(), minTime = getViewState()->minTimeSpanMs();
  const int minCol = maxTime > 0 ? std::max(1, int(std::ceil(W * tf / maxTime - 1e-9))) : 1;
  const int maxCol = std::min(chart_raster::maxColumnPixels(tf),
                              minTime > 0 ? std::max(1, int(std::floor(W * tf / minTime + 1e-9)))
                                          : chart_raster::kMaxCellPx);
  return {minCol, maxCol};
}

double ZoomController::clampSpan(double span, double current, double maximum, double minimum) {
  if (maximum > 0) span = std::min(span, maximum);
  if (minimum > 0) span = std::max(span, std::min(current, minimum));
  return span;
}

double ZoomController::clampSpan(double span, double current, double maximum, double minimum, int clicks) {
  if (clicks < 0 && maximum > 0) span = std::min(span, std::max(current, maximum));
  if (clicks > 0 && minimum > 0) span = std::max(span, std::min(current, minimum));
  return span;
}

void ZoomController::postZoom(bool price) {
  if (price) emit getViewState()->priceInteracted();
  if (getViewState()->isAutoScrollEnabled()) controller_.livePlacement().enableAutoScroll(false);
}

chart_raster::RowRung ZoomController::clickPriceFallback(chart_raster::RowRung row,
    chart_raster::RowRung baseRow, int clicks, bool price) const {
  if (price && row == baseRow) {
    // No rows to whole-pixel (no tick drawn yet) or no rung (rows at the cell cap, e.g.
    // a carried window before the new symbol's price): the price span by the ratio, in
    // the limit of the click's direction (encoded as row {-span, 0}).
    const double current = getViewState()->getMaxPrice() - getViewState()->getMinPrice();
    double span = current / std::pow(chart_raster::kZoomStepRatio, double(clicks));
    span = clampSpan(span, current, getViewState()->maxPriceSpan(), getViewState()->minPriceSpan(), clicks);
    if (span != current && std::isfinite(span) && span > 0) row = {-span, 0};
  }
  return row;
}

ZoomController::ClickTarget ZoomController::clickTarget(const chart_raster::RasterCamera& drawn,
    int clicks, QPointF point, bool time, bool price) const {
  const double W = drawn.widthDev, H = drawn.heightDev, tf = double(controller_.session().timeframeMs);
  const auto [fracX, fracY] = zoomFrac(point.x(), point.y());
  // Rapid clicks step on from the glide's target, so they accumulate monotonically.
  const int baseCol = controller_.glide().time.active ? controller_.glide().colPx : drawn.colPx;
  const chart_raster::RowRung baseRow =
      controller_.glide().price.active ? controller_.glide().row : chart_raster::RowRung{drawn.tick, drawn.rowPx};
  const auto [minCol, maxCol] = columnLimits(W, tf);
  const int col = time ? chart_raster::columnRung(baseCol, clicks, minCol, maxCol) : baseCol;
  const qint64 targetSpan = col > 0 && tf > 0 ? std::llround(W * tf / double(col))
                                              : getViewState()->getVisibleTimeEnd() - getViewState()->getVisibleTimeStart();
  const double at = drawn.timeAtXDev(fracX * W), ap = drawn.priceAtYDev(fracY * H);
  const qint64 targetStart = std::llround(at - fracX * double(targetSpan));
  // Auto's tick for a price rung is predicted over exactly the time bounds this click
  // commits (the frame chooses the tick over the stored view): the target's when time
  // zooms, else the stored bounds unchanged (not the drawn columns).
  const bool timeMoves = time && col != baseCol;
  const qint64 tickStart = timeMoves ? targetStart : getViewState()->getVisibleTimeStart();
  const qint64 tickEnd = timeMoves ? targetStart + targetSpan : getViewState()->getVisibleTimeEnd();
  chart_raster::RowRung row = baseRow;
  if (price && environment_.drawnTick() > 0)
    row = chart_raster::rowRung(baseRow, clicks, int(H), getViewState()->minPriceSpan(), getViewState()->maxPriceSpan(),
                                controller_.camera().tickPredictor(tickStart, tickEnd, ap, fracY));
  row = clickPriceFallback(row, baseRow, clicks, price);
  const bool zoomTime = timeMoves, zoomPrice = price && !(row == baseRow);
  return {fracX, fracY, baseCol, baseRow, col, row, at, ap, zoomTime, zoomPrice};
}

bool ZoomController::setZoomRung(const chart_raster::RasterCamera& drawn, double fracX, double fracY,
                                      bool time, int colPx, bool price, chart_raster::RowRung row) {
  if (!getViewState() || !drawn.valid) return false;
  qint64 start = getViewState()->getVisibleTimeStart(), end = getViewState()->getVisibleTimeEnd();
  double lo = getViewState()->getMinPrice(), hi = getViewState()->getMaxPrice();
  const double tf = double(controller_.session().timeframeMs);
  if (time && colPx > 0 && tf > 0) {
    const int64_t span = std::max<int64_t>(1, std::llround(double(drawn.widthDev) * tf / double(colPx)));
    const double at = drawn.timeAtXDev(fracX * double(drawn.widthDev));
    start = std::llround(at - fracX * double(span));
    end = start + span;
  }
  // row.tick < 0: no tick drawn yet, -row.tick is the price span itself (no rung).
  if (price && ((row.rowPx > 0 && row.tick > 0) || row.tick < 0)) {
    const double span = row.tick < 0 ? -row.tick : double(drawn.heightDev) * row.tick / double(row.rowPx);
    const double ap = drawn.priceAtYDev(fracY * double(drawn.heightDev));
    hi = ap + fracY * span;
    lo = hi - span;
  }
  const auto anchor = getViewState()->rasterAnchor();
  getViewState()->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  getViewState()->setViewport(start, end, lo, hi); // one change; the auto price fit applies inside
  return true;
}

void ZoomController::zoomStoredView(double factor, double x, double y, bool time, bool price) {
  const auto [fracX, fracY] = zoomFrac(x, y);
  qint64 start = getViewState()->getVisibleTimeStart(), end = getViewState()->getVisibleTimeEnd();
  double lo = getViewState()->getMinPrice(), hi = getViewState()->getMaxPrice();
  if (time) {
    double span = double(end - start) / factor;
    span = clampSpan(span, double(end - start), getViewState()->maxTimeSpanMs(), getViewState()->minTimeSpanMs());
    const double at = double(start) + fracX * double(end - start);
    const int64_t s = std::max<int64_t>(1, std::llround(span));
    start = std::llround(at - fracX * double(s));
    end = start + s;
  }
  if (price && hi > lo) {
    double span = (hi - lo) / factor;
    span = clampSpan(span, hi - lo, getViewState()->maxPriceSpan(), getViewState()->minPriceSpan());
    const double ap = hi - fracY * (hi - lo);
    hi = ap + fracY * span;
    lo = hi - span;
  }
  sLog_Probe("zoom.click", "no camera: the stored view x" << factor << " time=" << time << " price=" << price);
  if (start == getViewState()->getVisibleTimeStart() && end == getViewState()->getVisibleTimeEnd() &&
      lo == getViewState()->getMinPrice() && hi == getViewState()->getMaxPrice())
    return; // at the limits
  const auto anchor = getViewState()->rasterAnchor();
  getViewState()->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  getViewState()->setViewport(start, end, lo, hi);
  postZoom(price);
  emit updateRequested();
}

void ZoomController::zoomClicks(int clicks, double x, double y, bool time, bool price) {
  if (!getViewState() || !environment_.attached() || !getViewState()->isTimeWindowValid() || clicks == 0 ||
      getViewState()->isDragging())
    return;
  if (controller_.glide().gesture) endZoomGesture();
  // The auto price scale owns price: the chart wheel zooms time only.
  price = price && !getViewState()->autoPriceScale();
  const auto drawn = controller_.camera().rasterCameraNow(false); // what is on screen now (a glide in flight included)
  if (!time && !price) return;
  if (!drawn.valid) {
    // Nothing drawn yet (no tick or size): the stored view by the ratio, no glide.
    zoomStoredView(std::pow(chart_raster::kZoomStepRatio, double(clicks)), x, y, time, price);
    return;
  }
  const auto target = clickTarget(drawn, clicks, {x, y}, time, price);
  const auto [fracX, fracY, baseCol, baseRow, col, row, at, ap, zoomTime, zoomPrice] = target;
  if (!zoomTime && !zoomPrice) {
    sLog_Probe("zoom.click", "at the limits clicks=" << clicks << " time=" << time << " price=" << price << " col="
               << baseCol << " row=" << baseRow.rowPx << "@" << baseRow.tick << " span="
               << getViewState()->getMaxPrice() - getViewState()->getMinPrice() << " limits=" << getViewState()->minPriceSpan()
               << ".." << getViewState()->maxPriceSpan());
    return;
  }
  sLog_Probe("zoom.click", "clicks=" << clicks << " col=" << baseCol << "->" << col << " row=" << baseRow.rowPx
             << "@" << baseRow.tick << "->" << row.rowPx << "@" << row.tick);
  const auto restBefore = controller_.camera().restCameraNow(false);
  if (!setZoomRung(drawn, fracX, fracY, zoomTime, col, zoomPrice, row)) return;
  postZoom(zoomPrice);
  // A new glide from what is drawn on every axis whose end the click changed (the auto
  // price fit of the new time window included); the other axis is left as it is.
  controller_.zoomGlide().startAxisGlides(drawn, restBefore, at, ap, false, false);
}

bool ZoomController::canZoomContinuous(double factor) const {
  if (!getViewState() || !environment_.attached() || !getViewState()->isTimeWindowValid() || !std::isfinite(factor) ||
      !(factor > 0) || factor == 1.0 || getViewState()->isDragging())
    return false;
  return true;
}

void ZoomController::applyContinuousView(const chart_raster::RasterCamera& cam, double factor,
    double fracX, double fracY, Axes axes, Axes joins) {
  const auto [time, price] = axes;
  const auto [timeJoins, priceJoins] = joins;
  const double W = cam.widthDev, H = cam.heightDev;
  const double at = cam.timeAtXDev(fracX * W), ap = cam.priceAtYDev(fracY * H);
  qint64 start = timeJoins ? std::llround(cam.drawnStartMs) : getViewState()->getVisibleTimeStart();
  qint64 end = timeJoins ? std::llround(cam.drawnEndMs) : getViewState()->getVisibleTimeEnd();
  double lo = priceJoins ? cam.drawnMinPrice : getViewState()->getMinPrice();
  double hi = priceJoins ? cam.drawnMaxPrice : getViewState()->getMaxPrice();
  if (time) {
    // Inside the zoom limits (one column per pixel out; the zoom-in floor in).
    const double maxTime = getViewState()->maxTimeSpanMs(), minTime = getViewState()->minTimeSpanMs();
    double span = double(end - start) / factor;
    span = clampSpan(span, double(end - start), maxTime, minTime);
    const int64_t s = std::max<int64_t>(1, std::llround(span));
    start = std::llround(at - fracX * double(s));
    end = start + s;
  }
  if (price) {
    const double maxPrice = getViewState()->maxPriceSpan(), minPrice = getViewState()->minPriceSpan();
    double span = (hi - lo) / factor;
    span = clampSpan(span, hi - lo, maxPrice, minPrice);
    hi = ap + fracY * span;
    lo = hi - span;
  }
  const auto anchor = getViewState()->rasterAnchor();
  getViewState()->setRasterAnchor(time ? fracX : anchor.fracX, price ? fracY : anchor.fracY);
  getViewState()->setViewport(start, end, lo, hi);
  postZoom(price);
}

void ZoomController::zoomContinuous(double factor, double x, double y, bool time, bool price) {
  zoomContinuousFor(factor, x, y, time, price, kGestureSettleMs);
}

void ZoomController::zoomContinuousFor(double factor, double x, double y, bool time, bool price,
                                            int settleMs) {
  if (!canZoomContinuous(factor)) return;
  price = price && !getViewState()->autoPriceScale();
  if (!time && !price) return;
  const auto [fracX, fracY] = zoomFrac(x, y);
  // The camera on screen: before the gesture what is drawn (a rest view or a glide),
  // later the continuous camera on the axes the gesture zooms. An axis joins the
  // gesture from its drawn window, so its continuous camera starts exactly on the
  // picture (no jump), in one viewport change; an axis the gesture does not zoom keeps
  // its stored bounds and its whole pixels.
  auto& glide = controller_.glide();
  const bool starting = !glide.gesture;
  const auto cam = controller_.camera().rasterCameraNow(false);
  if (!cam.valid) {
    if (!glide.gesture) zoomStoredView(factor, x, y, time, price); // nothing drawn yet
    return;
  }
  if (starting) {
    glide.gesture = true;
    glide.gestureTime = glide.gesturePrice = false;
    sLog_Probe("zoom.gesture", "begin");
  }
  const bool timeJoins = time && !glide.gestureTime, priceJoins = price && !glide.gesturePrice;
  // An axis joining the gesture leaves its glide where it is drawn; a glide on the other
  // axis keeps running.
  controller_.zoomGlide().endZoomGlide(timeJoins, priceJoins);
  glide.gestureTime = glide.gestureTime || time;
  glide.gesturePrice = glide.gesturePrice || price;
  gestureAt_ = QPointF(x, y);
  applyContinuousView(cam, factor, fracX, fracY, {time, price}, {timeJoins, priceJoins});
  if (!settleTimer_) {
    settleTimer_ = timerFactory_([this] { endZoomGesture(); });
  }
  settleTimer_->start(settleMs);
  emit updateRequested();
}

void ZoomController::endZoomGesture() {
  if (settleTimer_) settleTimer_->stop();
  if (!controller_.glide().gesture) return;
  const auto from = controller_.camera().rasterCameraNow(false); // the continuous camera on the gesture's axes, as drawn
  const auto restBefore = controller_.camera().restCameraNow(false);
  controller_.glide().gesture = false;
  sLog_Probe("zoom.gesture", "end");
  if (!from.valid || !getViewState()) {
    emit updateRequested();
    return;
  }
  // Settle on the nearest rung about the last gesture point.
  const double W = from.widthDev, H = from.heightDev, tf = double(controller_.session().timeframeMs);
  const auto [fracX, fracY] = zoomFrac(gestureAt_.x(), gestureAt_.y());
  const auto [minCol, maxCol] = columnLimits(W, tf);
  // An axis the gesture did not zoom keeps its drawn whole pixels.
  const int col = controller_.glide().gestureTime ? chart_raster::nearestColumnRung(from.colPxF, minCol, maxCol) : from.colPx;
  const qint64 span = std::llround(W * tf / double(col));
  const double at = from.timeAtXDev(fracX * W), ap = from.priceAtYDev(fracY * H);
  const qint64 start = std::llround(at - fracX * double(span));
  const bool price = controller_.glide().gesturePrice && !getViewState()->autoPriceScale() && environment_.attached() && environment_.drawnTick() > 0;
  // The tick predicted over the time bounds committed below (the stored ones when the
  // gesture did not zoom time).
  const qint64 tickStart = controller_.glide().gestureTime ? start : getViewState()->getVisibleTimeStart();
  const qint64 tickEnd = controller_.glide().gestureTime ? start + span : getViewState()->getVisibleTimeEnd();
  chart_raster::RowRung row{from.tick, from.rowPx};
  if (price)
    row = chart_raster::nearestRowRung(from.pxPerPrice(), int(H), getViewState()->minPriceSpan(),
                                       getViewState()->maxPriceSpan(), controller_.camera().tickPredictor(tickStart, tickEnd, ap, fracY));
  if (!setZoomRung(from, fracX, fracY, controller_.glide().gestureTime, col, price && row.rowPx > 0, row)) {
    emit updateRequested();
    return;
  }
  // The gesture's axes glide from their continuous camera to the rung; the other axis
  // is left as it is.
  controller_.zoomGlide().startAxisGlides(from, restBefore, at, ap, controller_.glide().gestureTime, controller_.glide().gesturePrice);
}

void ZoomController::wheelZoom(int angle, bool pixelDeltaNull, int phase, bool precise,
                                    double x, double y, bool time, bool price, WheelRoute route) {
  const auto kind = WheelInterpreter::classify(angle, pixelDeltaNull, phase, precise);
  if (kind == WheelInterpreter::Kind::Notch) {
    // A partial notch carries to the next event of the same route and axes, in the same
    // direction; at most four clicks per event, and only the partial notch is kept.
    const int key = int(route) * 4 + (time ? 2 : 0) + (price ? 1 : 0);
    if (key != wheelCarryKey_ || (wheelAngleRemainder_ > 0 && angle < 0) || (wheelAngleRemainder_ < 0 && angle > 0))
      wheelAngleRemainder_ = 0;
    wheelCarryKey_ = key;
    wheelAngleRemainder_ += angle;
    const int whole = wheelAngleRemainder_ / 120;
    wheelAngleRemainder_ -= whole * 120;
    const int clicks = std::clamp(whole, -4, 4);
    if (clicks) zoomClicks(clicks, x, y, time, price);
  } else if (kind == WheelInterpreter::Kind::End) {
    endZoomGesture();
  } else if (kind == WheelInterpreter::Kind::Continuous) {
    zoomContinuousFor(continuousFactor(angle), x, y, time, price, kGestureSettleMs);
  }
}

void ZoomController::zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x) {
  wheelZoom(angleDelta, pixelDelta == 0, phase, false,
            x, height() / 2, true, false, WheelRoute::TimeAxis);
}

void ZoomController::zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y) {
  if (getViewState() && angleDelta != 0) getViewState()->setAutoPriceScale(false);
  wheelZoom(angleDelta, pixelDelta == 0, phase, false,
            width() / 2, y, false, true, WheelRoute::PriceAxis);
}

void ZoomController::zoomTimeDrag(double delta, double x) {
  zoomContinuousFor(continuousFactor(delta), x, height() / 2, true, false, kGestureSafetyMs);
}

void ZoomController::zoomPriceDrag(double delta, double y) {
  if (getViewState()) getViewState()->setAutoPriceScale(false);
  zoomContinuousFor(continuousFactor(delta), width() / 2, y, false, true, kGestureSafetyMs);
}

void ZoomController::zoomTimeClicks(int clicks, double x) { zoomClicks(clicks, x, height() / 2, true, false); }

void ZoomController::zoomPriceClicks(int clicks, double y) {
  if (getViewState()) getViewState()->setAutoPriceScale(false);
  zoomClicks(clicks, width() / 2, y, false, true);
}

} // namespace chart
