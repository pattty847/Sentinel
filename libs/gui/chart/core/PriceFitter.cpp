#include "chart/core/PriceFitter.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/CandleInterval.hpp"
#include "SentinelLogging.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace chart {
namespace {
bool skipBar(const CandleBar& bar, int64_t tf, double drawnStart, double drawnEnd) {
    const qint64 barEnd = bar.timeEndMs > bar.timeStartMs ? bar.timeEndMs : bar.timeStartMs + tf;
    return double(bar.timeStartMs) >= drawnEnd || double(barEnd) <= drawnStart || !std::isfinite(bar.high) ||
        !std::isfinite(bar.low) || !(bar.low > 0) || bar.high < bar.low || !drawsCandle(tf, bar.volume);
}
} // namespace

PriceFitter::PriceFitter(ChartViewController& controller, IChartEnvironment& environment, ITickOracle& ticks)
    : controller_(controller), environment_(environment), ticks_(ticks) {}

qint64 PriceFitter::gpuLiveEndMs(qint64 spanMs) const {
  const int64_t anchor = environment_.attached() ? environment_.liveAnchorMs() : 0;
  const int64_t tf = controller_.session().timeframeMs;
  if (anchor <= 0 || tf <= 0) return 0;
  const int64_t liveEnd = recording::floorDiv(anchor + tf - 1, tf) * tf;
  return liveEnd + std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(spanMs) * kAutoScrollPaddingFrac));
}

std::optional<std::pair<qint64, qint64>> PriceFitter::gpuFitTimeWindow() const {
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  const int64_t anchor = environment_.liveAnchorMs();
  if (anchor <= 0) return std::nullopt;
  int64_t oldest = environment_.oldestAvailableMs();
  if (oldest <= 0 && candles_)
    oldest = candles_->oldestTimeMs(controller_.session().symbol, tf);
  const int64_t dataEnd = recording::floorDiv(anchor + tf - 1, tf) * tf; // the live bucket's end
  const int64_t dataStart = oldest > 0 ? recording::floorDiv(oldest, tf) * tf : 0;
  const double maxTime = controller_.viewPtr()->maxTimeSpanMs();
  const int64_t maxSpan = maxTime > 0 ? std::max<int64_t>(1, static_cast<int64_t>(std::floor(maxTime))) : INT64_MAX / 4;
  const int64_t minSpan = std::max<int64_t>(tf, static_cast<int64_t>(std::ceil(controller_.viewPtr()->minTimeSpanMs())));
  const double pad = kAutoScrollPaddingFrac;
  // The span that shows [dataStart, dataEnd] plus the follow-live padding
  // (max(tf, span * pad)) after it.
  int64_t span = maxSpan;
  bool fits = false;
  if (dataStart > 0 && dataEnd > dataStart) {
    const double dataSpan = static_cast<double>(dataEnd - dataStart);
    const double padded = pad > 0 && pad < 1 && dataSpan / (1 - pad) * pad >= double(tf) ? dataSpan / (1 - pad)
                                                                                          : dataSpan + double(tf);
    const int64_t wanted = static_cast<int64_t>(std::ceil(padded));
    fits = wanted <= maxSpan;
    span = std::min(wanted, maxSpan);
  }
  span = std::max(span, std::min(minSpan, maxSpan));
  const bool following = controller_.viewPtr()->isAutoScrollEnabled();
  if (fits || following || dataStart <= 0) {
    const int64_t end = gpuLiveEndMs(span);
    if (end <= 0) return std::nullopt;
    return std::make_pair<qint64, qint64>(end - span, qint64(end));
  }
  // History that does not fit: keep the view centre, inside the data.
  const int64_t centre = controller_.viewPtr()->getVisibleTimeStart() +
                         (controller_.viewPtr()->getVisibleTimeEnd() - controller_.viewPtr()->getVisibleTimeStart()) / 2;
  const int64_t lastStart = gpuLiveEndMs(span) - span;
  const int64_t start = std::clamp<int64_t>(centre - span / 2, dataStart, std::max(dataStart, lastStart));
  return std::make_pair<qint64, qint64>(qint64(start), start + span);
}

std::optional<std::pair<double, double>> PriceFitter::gpuFitPriceWindow(qint64 start, qint64 end,
                                                                     bool candlesOnly,
                                                                     std::optional<double> anchorFracX) {
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  PriceRange range;
  auto& [lo, hi, centre, span] = range;
  // The candles the window draws: the raster camera's time window (wider or narrower
  // than [start, end] by the column rounding), as the candle overlay selects them.
  const auto [drawnStart, drawnEnd] = controller_.camera().drawnTimeWindow(start, end, anchorFracX);
  const auto [used, newestClose] = collectVisibleBars(tf, drawnStart, drawnEnd, lo, hi);
  const bool onePrice = used && hi == lo;
  const double current = controller_.viewPtr()->getMaxPrice() - controller_.viewPtr()->getMinPrice();
  if (!centreAndSpan(range, used, current, candlesOnly)) return std::nullopt;
  applySpanLimits(range, used, newestClose);
  return snapToRows(range, {start, end, drawnStart, drawnEnd, onePrice, current});
}

PriceFitter::VisibleBars PriceFitter::collectVisibleBars(int64_t tf, double drawnStart, double drawnEnd,
                                                       double& lo, double& hi) {
  const qint64 first = static_cast<qint64>(std::floor(drawnStart)), last = static_cast<qint64>(std::ceil(drawnEnd));
  // From the bucket that contains the view start: visibleBars selects by bar
  // start, and the candle that starts before the view and ends inside it counts.
  const int64_t alignedStart = recording::floorDiv(first, tf) * tf;
  auto& bars = bars_; // reused: the auto price scale runs this on every change in view
  bars.clear();
  if (candles_ && last > first)
    candles_->visibleBars(controller_.session().symbol, tf, alignedStart, last, bars);
  size_t used = 0;
  double newestClose = 0;
  qint64 newestStart = 0;
  for (const auto& bar : bars) {
    if (skipBar(bar, tf, drawnStart, drawnEnd))
      continue;
    lo = used ? std::min(lo, bar.low) : bar.low;
    hi = used ? std::max(hi, bar.high) : bar.high;
    if (!used || bar.timeStartMs >= newestStart) {
      newestStart = bar.timeStartMs;
      newestClose = std::isfinite(bar.close) && bar.close > 0 ? bar.close : (bar.low + bar.high) * 0.5;
    }
    ++used;
  }
  return {used, newestClose};
}

bool PriceFitter::centreAndSpan(PriceRange& range, size_t used, double current, bool candlesOnly) const {
  auto& [lo, hi, centre, span] = range;
  if (used) {
    centre = (lo + hi) * 0.5;
    span = hi > lo ? (hi - lo) * (1.0 + 2.0 * kFitPriceMargin) : current;
  } else {
    if (candlesOnly) return false;
    centre = controller_.gpuLivePrice();
    span = current;
  }
  if (!(centre > 0) || !std::isfinite(centre)) return false;
  return true;
}

void PriceFitter::applySpanLimits(PriceRange& range, size_t used, double newestClose) const {
  auto& [lo, hi, centre, span] = range;
  if (!(span > 0) || !std::isfinite(span) || span > centre * 4) span = centre * 0.01; // e.g. a span from another symbol
  // A minimum span wider than the price itself is another symbol's (the layer's limits
  // follow the new symbol's data a moment after a switch): not applied.
  if (const double minSpan = controller_.viewPtr()->minPriceSpan(); minSpan < centre) span = std::max(span, minSpan);
  const double maxSpan = controller_.viewPtr()->maxPriceSpan();
  if (maxSpan > 0 && span > maxSpan) {
    // Manual tick: the user's tick stays (never Auto,
    // never coarsened); the candles need more than one row per pixel, so show what
    // the limit can, centred on the current price: book mid, last trade, newest close.
    span = maxSpan;
    if (used) {
      const double now = controller_.session().bookMid > 0 ? controller_.session().bookMid : controller_.session().lastTrade > 0 ? controller_.session().lastTrade : newestClose;
      if (now > 0) centre = now;
    }
  }
  lo = centre - span * 0.5;
  hi = centre + span * 0.5;
}

std::pair<double, double> PriceFitter::snapToRows(PriceRange& range, const FitContext& context) {
  auto& [lo, hi, centre, span] = range;
  const auto& [start, end, drawnStart, drawnEnd, onePrice, current] = context;
  // Whole pixels per row (raster camera): the fitted span is heightDev * tick / P for
  // an integer P, its top on a row edge, so the drawn window equals the fitted one
  // (no snap crops the candles). Before a tick is drawn the span stays.
  const double dpr = environment_.surface().dpr;
  const int H = chart_raster::devicePixels(environment_.surface().heightLogical, dpr);
  const double tick = environment_.attached() ? environment_.drawnTick() : 0.0;
  if (controller_.viewPtr()->autoPriceScale() && environment_.attached() && !environment_.manualTick()) {
    // Startup and symbol/timeframe switches can lack a current resolution; a fit
    // needs it, while a failed solve after resolution arrives warrants a warning.
    if (!environment_.hasCurrentResolution()) return std::make_pair(lo, hi);
    if (onePrice && current > 0 && std::isfinite(current)) {
      return onePriceRows(centre, current);
    }
    solveAutoRows(range, context, H, tick);
  } else {
    chart_raster::fitPriceManualRows(lo, hi, H, tick);
  }
  return std::make_pair(lo, hi);
}

std::pair<double, double> PriceFitter::onePriceRows(double centre, double current) const {
  // One price (hi == lo) has no new scale information. Keep the span and
  // leave the tick decision/proposal untouched; translate by drawn whole rows.
  double low = controller_.viewPtr()->getMinPrice(), high = controller_.viewPtr()->getMaxPrice();
  const double row = environment_.drawnTick();
  const double margin = current * kFitPriceMargin;
  if ((centre <= low + margin || centre >= high - margin) && std::isfinite(row) && row > 0) {
    const double shift = std::round((centre - (low + high) * .5) / row) * row;
    low += shift;
    high += shift;
  }
  return std::make_pair(low, high);
}

void PriceFitter::solveAutoRows(PriceRange& range, const FitContext& context, int H, double tick) {
  auto& [lo, hi, centre, span] = range;
  const auto& [start, end, drawnStart, drawnEnd, onePrice, current] = context;
  // Solve from the raw candle bounds and check coverage for each candidate's
  // fitted window; publish its tick before setViewport notifies the renderer.
  const auto &settings = ticks_.autoTickParams();
  ++autoPriceSolveCount_;
  const auto fit = chart_raster::solveAutoPriceFit(lo, hi, H, ticks_.autoTickState(), environment_.priceScale(),
      {settings.minRowPx, settings.hysteresis}, [&](int64_t units, double low, double high) {
        return ticks_.buildsTick(units, {drawnStart, drawnEnd, low, high});
      });
  if (ticks_.setAutoPriceTick(fit ? std::optional(fit->tickUnits) : std::nullopt)) requestUpdate_();
  if (fit) {
    sLog_Probe("zoom.fit", "raw=[" << lo << ".." << hi << "] fit=[" << fit->lo << ".." << fit->hi
               << "] tick=" << fit->tick << " P=" << fit->rowPx);
    lo = fit->lo;
    hi = fit->hi;
  } else {
    sLog_Warning("auto price fit unavailable; publishing plain fit time=[" << start << ".." << end
                 << "] raw=[" << lo << ".." << hi << "]");
    chart_raster::fitPriceToRows(lo, hi, H, tick);
  }
}

bool PriceFitter::autoPriceFit(qint64 start, qint64 end, double& priceMin, double& priceMax) {
  ++autoPriceFitRevision_;
  if (!environment_.attached() || !controller_.viewPtr()) return false;
  const auto window = gpuFitPriceWindow(start, end, true);
  if (!window) {
    if (ticks_.setAutoPriceTick(std::nullopt)) requestUpdate_();
    return false; // no visible candle: the given price stays
  }
  std::tie(priceMin, priceMax) = *window;
  controller_.flags().markPriceKnown();
  priceCarry_.reset();
  return true;
}

void PriceFitter::refitAutoPrice() {
  if (!controller_.viewPtr() || !controller_.viewPtr()->autoPriceScale() || !controller_.viewPtr()->isTimeWindowValid() ||
      controller_.flags().limitsDeferred)
    return;
  const bool known = controller_.flags().priceKnown;
  controller_.setGpuViewportSelf(controller_.viewPtr()->getVisibleTimeStart(), controller_.viewPtr()->getVisibleTimeEnd(), controller_.viewPtr()->getMinPrice(),
                     controller_.viewPtr()->getMaxPrice(), true);
  if (controller_.flags().priceKnown != known) controller_.syncGpuView();
}

void PriceFitter::candlesDirty(const QString& symbol, int64_t timeframeMs, qint64 dirtyStart, qint64 dirtyEnd) {
  if (!controller_.viewPtr() || !controller_.viewPtr()->autoPriceScale() ||
      !controller_.viewPtr()->isTimeWindowValid() || symbol != controller_.session().symbol)
    return;
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  if (timeframeMs != tf) return;
  // The window drawn on screen (a drag commits only at release).
  const auto [shownStart, shownEnd] = controller_.viewPtr()->displayedTimeWindow();
  const auto [drawnStart, drawnEnd] = controller_.camera().drawnTimeWindow(shownStart, shownEnd);
  const qint64 viewStart =
      recording::floorDiv(static_cast<qint64>(std::floor(drawnStart)), tf) * tf;
  if (std::max(dirtyEnd, dirtyStart + tf) <= viewStart || double(dirtyStart) >= drawnEnd)
    return;
  refitAutoPrice();
}

} // namespace chart
