#include "chart/core/LivePlacement.hpp"
#include "chart/core/ChartViewController.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "SentinelLogging.hpp"
#include <QDateTime>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace chart {
namespace {
class BootstrapTimer final : public ILivePlacementTimer {
public:
    explicit BootstrapTimer(std::function<void()> timeout) {
        QObject::connect(&timer_, &QTimer::timeout, &timer_, std::move(timeout));
    }
    void setInterval(int ms) override { timer_.setInterval(ms); }
    bool isActive() const override { return timer_.isActive(); }
    void start() override { timer_.start(); }
    void stop() override { timer_.stop(); }
private:
    QTimer timer_;
};
}

LivePlacement::LivePlacement(ChartViewController& controller, IChartEnvironment& environment)
    : controller_(controller), environment_(environment), clock_(QDateTime::currentMSecsSinceEpoch),
      timerFactory_([](std::function<void()> timeout) { return std::make_unique<BootstrapTimer>(std::move(timeout)); }) {}

int64_t LivePlacement::liveEndCeil(int64_t x, int64_t tf) {
    return recording::floorDiv(x + tf - 1, tf) * tf;
}

void LivePlacement::setTimingForTest(std::function<qint64()> clock, TimerFactory timerFactory) {
    bootstrapTimer_.reset();
    clock_ = std::move(clock);
    timerFactory_ = std::move(timerFactory);
}

void LivePlacement::stopBootstrap() {
    if (bootstrapTimer_) bootstrapTimer_->stop();
}

// Follow-live keeps the view end one padding past the live bucket's end.
void LivePlacement::followGpuLive() {
  if (environment_.attached()) followGpuLiveTo(environment_.liveOpenEndMs());
}

void LivePlacement::followGpuLiveTo(int64_t openEnd) {
  if (!environment_.attached() || !controller_.viewPtr() || !controller_.viewPtr()->isTimeWindowValid() ||
      !controller_.viewPtr()->isAutoScrollEnabled() || controller_.viewPtr()->isDragging())
    return;
  const int64_t tf = controller_.session().timeframeMs;
  if (openEnd <= 0 || tf <= 0) return;
  const int64_t start = controller_.viewPtr()->getVisibleTimeStart(), end = controller_.viewPtr()->getVisibleTimeEnd();
  const int64_t span = end - start;
  const int64_t liveEnd = liveEndCeil(openEnd, tf);
  // The padding a timeframe switch kept (the Now column where it was drawn), while the
  // span and timeframe are the switch's; else the nominal one.
  // Only while the view is still the placement the switch made (follow updates move it
  // along); any other write of the time window (return to live, reset, the API, an
  // explicit view, a pan) is a new placement with the nominal padding.
  const bool kept = keepsFollowPadding(start, end, tf);
  const int64_t pad = kept ? followPadding_->padMs
                           : std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) *
                                                                         kAutoScrollPaddingFrac));
  const int64_t target = liveEnd + pad;
  // Whole buckets, and nothing while the end is within one drawn pixel of the padding:
  // the drawn phase never changes, a live step moves the picture by whole columns.
  const auto cam = controller_.camera().restCameraNow(false);
  const int64_t shift = chart_raster::followShift(end, target, tf, cam.valid ? cam.colPx : 0);
  if (shift <= 0) return; // the live bucket is inside the padded view
  // A glide in flight on time moves with the view (the same world shift at both ends).
  controller_.zoomGlide().shiftTime(shift);
  controller_.setGpuViewportSelf(start + shift, end + shift, controller_.viewPtr()->getMinPrice(), controller_.viewPtr()->getMaxPrice(), true);
  if (kept) { // the follow update keeps the switch's placement
    followPadding_->startMs = controller_.viewPtr()->getVisibleTimeStart();
    followPadding_->endMs = controller_.viewPtr()->getVisibleTimeEnd();
  }
  emit liveRenderTick();
}

// The first price places the view; the loading hatch stays until the newest span lands.
// The price band uses initial_price_pct of kSeedBandRows rows at kSeedBandTick.
void LivePlacement::seedGpuViewport(double bestBid, double bestAsk) {
  if (!shouldSeed(bestBid, bestAsk)) return;
  const bool timeValid = controller_.viewPtr()->isTimeWindowValid();
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  qint64 start = controller_.viewPtr()->getVisibleTimeStart(), end = controller_.viewPtr()->getVisibleTimeEnd();
  if (!timeValid) {
    const int64_t span = gpuInitialSpanMs(environment_.surface().widthLogical);
    const int64_t now = clock_();
    controller_.flags().pristine = true;
    end = liveEndCeil(now, tf) +
          std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * kAutoScrollPaddingFrac));
    start = end - span;
  }
  const int pct = controller_.initialViewSettings().pricePct;
  const double full = kSeedBandTick * kSeedBandRows;
  const double span = pct > 0 && pct < 100 ? full * pct / 100.0 : full;
  const double mid = (bestBid + bestAsk) * 0.5;
  const double lo = mid - span * 0.5, hi = mid + span * 0.5;
  const bool carry = controller_.priceFitter().priceCarry() && timeValid && controller_.flags().reseedPrice;
  if (!carry) controller_.priceFitter().priceCarry().reset();
  controller_.flags().markPriceKnown();
  sLog_Render("GPU heatmap viewport seeded from the book top: mid=" << mid << " time=[" << start << ".." << end
              << "] carry=" << carry << " autoPriceScale=" << controller_.viewPtr()->autoPriceScale());
  // A symbol switch with auto price scale off: the carried zoom; else the default band
  // (auto price scale on: the visible candles win inside setViewport).
  // The raster camera's anchor: the seeded price centred, a seeded time at its end.
  controller_.viewPtr()->setRasterAnchor(timeValid ? controller_.viewPtr()->rasterAnchor().fracX : 1.0, 0.5);
  if (!carry || !applyPriceCarry_(mid)) controller_.setGpuViewportSelf(start, end, lo, hi);
  controller_.syncGpuView(); // also when the viewport did not change (priceKnown flips)
  emit updateRequested();
}

void LivePlacement::returnGpuToLive() {
  if (!environment_.attached() || !controller_.viewPtr() || !controller_.viewPtr()->isTimeWindowValid()) return;
  const int64_t span = controller_.viewPtr()->getVisibleTimeEnd() - controller_.viewPtr()->getVisibleTimeStart();
  const int64_t end = controller_.priceFitter().gpuLiveEndMs(span);
  if (end <= 0) return; // nothing known yet: the next live frame steps forward
  sLog_Render("GPU heatmap returns to live: anchor=" << environment_.liveAnchorMs() << " end=" << end);
  controller_.viewPtr()->setRasterAnchor(1.0, controller_.viewPtr()->rasterAnchor().fracY); // the live edge
  controller_.viewPtr()->setViewport(end - span, end, controller_.viewPtr()->getMinPrice(), controller_.viewPtr()->getMaxPrice());
  controller_.syncGpuView(); // publishes the view even when it was already there
  emit liveRenderTick();
}

void LivePlacement::bootstrapGpuTimeView() {
  if (!environment_.attached() || !controller_.viewPtr() || controller_.viewPtr()->isTimeWindowValid()) {
    if (bootstrapTimer_) bootstrapTimer_->stop();
    return;
  }
  const int64_t anchor = environment_.liveAnchorMs();
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  if (anchor <= 0) {
    // Availability arrives on the data thread: look again shortly (stops once
    // a view exists, from here, a book top, a trade or the API).
    if (!bootstrapTimer_) {
      bootstrapTimer_ = timerFactory_([this] { bootstrapGpuTimeView(); });
      bootstrapTimer_->setInterval(50);
    }
    if (!bootstrapTimer_->isActive()) bootstrapTimer_->start();
    return;
  }
  if (bootstrapTimer_) bootstrapTimer_->stop();
  const int64_t span = gpuInitialSpanMs(environment_.surface().widthLogical);
  const int64_t end = liveEndCeil(anchor, tf) +
                      std::max<int64_t>(tf, static_cast<int64_t>(static_cast<double>(span) * kAutoScrollPaddingFrac));
  sLog_Render("GPU heatmap time-only bootstrap from availability: anchor=" << anchor << " time=[" << end - span
              << ".." << end << "] (price from the first decoded data)");
  controller_.flags().priceKnown = false;
  controller_.flags().pristine = true;
  controller_.viewPtr()->setRasterAnchor(1.0, 0.5);
  controller_.setGpuViewportSelf(end - span, end, 0.0, 1.0); // placeholder price: nothing draws until it is known
  controller_.syncGpuView();
}

// The first view uses initial_column_px per column, with at least 16 columns.
qint64 LivePlacement::gpuInitialSpanMs(double widthPx) const {
  const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
  const double width = widthPx > 0.0 ? widthPx : 800.0;
  return static_cast<int64_t>(std::max(16, static_cast<int>(width / controller_.initialViewSettings().columnPx))) * tf;
}

void LivePlacement::enableAutoScroll(bool enabled) {
  if (!controller_.viewPtr()) return;
  controller_.viewPtr()->enableAutoScroll(enabled);
  if (enabled) returnGpuToLive(); // back to the live edge from history or the future, same span
  emit updateRequested();
  emit autoScrollEnabledChanged();
  sLog_Render("auto-scroll enabled=" << enabled << " reason=request");
}

void LivePlacement::setLiveBookTop(double bestBid, double bestAsk) {
  // The book top seeds the first viewport (and the price window after a symbol switch).
  if (std::isfinite(bestBid) && std::isfinite(bestAsk) && bestBid > 0 && bestAsk >= bestBid)
    controller_.session().bookMid = (bestBid + bestAsk) * 0.5;
  seedGpuViewport(bestBid, bestAsk);
}
bool LivePlacement::matchesUtf8Product(QStringView symbol, std::string_view product) {
  // QString::fromUtf8 consumes one initial BOM; the non-owning UTF-8 comparison
  // otherwise uses the same decoder, including replacement of malformed bytes.
  if (product.starts_with("\xef\xbb\xbf")) product.remove_prefix(3);
  return symbol == QUtf8StringView(product.data(), static_cast<qsizetype>(product.size()));
}

void LivePlacement::onTrade(const std::string& product, double price) {
  // The live price for auto-fit compares the ASCII product id without conversion.
  if (std::isfinite(price) && price > 0 &&
      controller_.session().symbol == QLatin1String(product.data(), static_cast<qsizetype>(product.size())))
    controller_.session().lastTrade = price;
  // A trade seeds the price window only when no book top has (yet) for this symbol.
  if ((controller_.flags().reseedPrice || !controller_.flags().priceKnown) && matchesUtf8Product(controller_.session().symbol, product))
    seedGpuViewport(price, price);
}

void LivePlacement::resize(double w) {
    if (controller_.flags().pristine && controller_.viewPtr() && controller_.viewPtr()->isTimeWindowValid() &&
        w > 0) {
      // Untouched seeded view: keep initial_column_px on the laid-out width.
      const qint64 end = controller_.viewPtr()->getVisibleTimeEnd();
      controller_.setGpuViewportSelf(end - gpuInitialSpanMs(w), end, controller_.viewPtr()->getMinPrice(),
                         controller_.viewPtr()->getMaxPrice());
    }
}

bool LivePlacement::keepsFollowPadding(int64_t start, int64_t end, int64_t tf) const {
  return followPadding_ && followPadding_->startMs == start && followPadding_->endMs == end &&
                    followPadding_->tfMs == tf && followPadding_->placement == controller_.viewPtr()->placementVersion();
}

bool LivePlacement::shouldSeed(double bestBid, double bestAsk) const {
  if (!controller_.viewPtr() || !std::isfinite(bestBid) || !std::isfinite(bestAsk) || bestBid <= 0 || bestAsk < bestBid) return false;
  const bool timeValid = controller_.viewPtr()->isTimeWindowValid();
  if (timeValid && controller_.flags().priceKnown && !controller_.flags().reseedPrice) return false;
  return true;
}

} // namespace chart
