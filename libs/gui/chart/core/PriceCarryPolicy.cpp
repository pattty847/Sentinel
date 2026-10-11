#include "chart/core/PriceCarryPolicy.hpp"
#include "chart/core/ChartViewController.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "SentinelLogging.hpp"
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace chart {
namespace {
class CarryTimer final : public IPriceCarryTimer {
public:
    explicit CarryTimer(std::function<void()> timeout) {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, &timer_, std::move(timeout));
    }
    bool isActive() const override { return timer_.isActive(); }
    void start(int ms) override { timer_.start(ms); }
    void stop() override { timer_.stop(); }
private:
    QTimer timer_;
};
}
PriceCarryPolicy::PriceCarryPolicy(ChartViewController& controller, IChartEnvironment& environment)
    : controller_(controller), environment_(environment),
      timerFactory_([](std::function<void()> timeout) { return std::make_unique<CarryTimer>(std::move(timeout)); }) {}

void PriceCarryPolicy::setTimerFactoryForTest(TimerFactory factory) {
    timer_.reset();
    timerFactory_ = std::move(factory);
}

void PriceCarryPolicy::stop() {
    if (timer_) timer_->stop();
}

// A symbol without its own price still shows an earlier symbol's bounds: retain
// its pending carry instead of recomputing against those bounds.
void PriceCarryPolicy::capture() {
    auto* view = controller_.viewPtr();
    auto& carry = controller_.priceFitter().priceCarry();
    if (!view || view->autoPriceScale() || !controller_.flags().reseedPrice) carry.reset();
    if (controller_.flags().priceKnown && !controller_.flags().reseedPrice && view && view->isTimeWindowValid() &&
        !view->autoPriceScale()) {
        const double now = capturePrice();
        const double lo = view->getMinPrice(), span = view->getMaxPrice() - lo;
        const PriceFitter::PriceCarry next{span / now, (now - lo) / span};
        if (now > 0 && span > 0 && std::isfinite(next.spanRatio) && next.spanRatio > 0 && std::isfinite(next.heightFrac))
            carry = next;
    }
}

double PriceCarryPolicy::capturePrice() {
    double now = controller_.gpuLivePrice();
    const auto* candles = controller_.priceFitter().candleSource();
    if (!(now > 0) && candles && candles->available()) {
        const int64_t tf = std::max<int64_t>(1, controller_.session().timeframeMs);
        auto* view = controller_.viewPtr();
        auto& bars = controller_.priceFitter().scratchBars();
        candles->visibleBars(controller_.session().symbol, std::max<int64_t>(1, (tf + 500) / 1000),
                             recording::floorDiv(view->getVisibleTimeStart(), tf) * tf,
                             view->getVisibleTimeEnd(), bars);
        for (auto it = bars.rbegin(); it != bars.rend() && !(now > 0); ++it)
            if (std::isfinite(it->close) && it->close > 0) now = it->close;
    }
    return now;
}

bool PriceCarryPolicy::apply(double now, bool liveOnly) {
    auto& carry = controller_.priceFitter().priceCarry();
    auto* view = controller_.viewPtr();
    if (!carry || !view || !(now > 0) || !std::isfinite(now)) return false;
    const double span = carry->spanRatio * now;
    const double lo = now - carry->heightFrac * span, hi = lo + span;
    if (!std::isfinite(span) || !(span > 0) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
        carry.reset();
        return false;
    }
    // Unknown limits leave the carry pending. A live-only symbol consumes it from
    // its live price after the wait, even if no recorded price scale ever arrives.
    const bool confirmed = liveOnly || (environment_.attached() && environment_.priceScaleCurrent());
    sLog_Render("price carry applied: now=" << now << " price=[" << lo << ".." << hi << "] confirmed=" << confirmed
                << " liveOnly=" << liveOnly);
    if (confirmed) {
        carry.reset();
        stop();
    } else {
        waitForScale();
    }
    controller_.setGpuViewportSelf(view->getVisibleTimeStart(), view->getVisibleTimeEnd(), lo, hi, true);
    return true;
}

void PriceCarryPolicy::waitForScale() {
    if (!timer_) timer_ = timerFactory_([this] { timeout(); });
    if (!timer_->isActive()) timer_->start(kCarryLiveOnlyWaitMs);
}

void PriceCarryPolicy::timeout() {
    if (environment_.attached() && controller_.priceFitter().priceCarry() && !controller_.flags().reseedPrice &&
        !environment_.priceScaleCurrent())
        apply(controller_.gpuLivePrice(), true);
}

void PriceCarryPolicy::limitsChanged() {
    if (controller_.priceFitter().priceCarry() && !controller_.flags().reseedPrice && !controller_.flags().limitsDeferred &&
        environment_.priceScaleCurrent())
        apply(controller_.gpuLivePrice());
}

void PriceCarryPolicy::priceInteracted() {
    controller_.priceFitter().priceCarry().reset(); // the user owns price now
}
} // namespace chart
