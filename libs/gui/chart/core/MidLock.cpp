#include "chart/core/MidLock.hpp"
#include "chart/core/ChartViewController.hpp"
#include "SentinelLogging.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace chart {

MidLock::MidLock(ChartViewController& controller, IChartEnvironment& environment)
    : controller_(controller), environment_(environment) {
    // The auto price scale and the lock own price exclusively: the scale turning on
    // (P, a price fit, the default view, the API) releases the lock.
    connect(controller_.viewPtr(), &GridViewState::autoPriceScaleChanged, this, [this] {
        if (controller_.viewPtr()->autoPriceScale()) release("auto price scale");
    });
    // Another route committed a price window (a chart zoom about the drawn centre, a
    // gesture's settle, a timeframe switch): the spring re-evaluates it without
    // waiting for the market (the lock's own writes and time-only moves do not wake it).
    connect(controller_.viewPtr(), &GridViewState::viewportChanged, this, [this] {
        const auto* view = controller_.viewPtr();
        if (view->getMinPrice() != writtenLo_ || view->getMaxPrice() != writtenHi_) wake();
    });
}

void MidLock::wake() {
    if (!enabled_ || moving_) return;
    moving_ = true;
    lastMs_ = -1;
    emit polishRequested();
}

void MidLock::setEnabled(bool on) {
    auto* view = controller_.viewPtr();
    if (enabled_ == on || !view) return;
    enabled_ = on;
    moving_ = false;
    view->setPriceLocked(on);
    if (on) {
        view->setAutoPriceScale(false); // the price window stays until the spring moves it
        view->setRasterAnchor(view->rasterAnchor().fracX, 0.5);
        writtenLo_ = writtenHi_ = std::numeric_limits<double>::quiet_NaN(); // adopt the stored window
        velocity_ = 0;
    }
    sLog_Render("lock to mid enabled=" << on << " target=" << target().value_or(0.0));
    emit enabledChanged();
    if (on) targetChanged();
}

void MidLock::release(const char* reason) {
    if (!enabled_) return;
    sLog_Render("lock to mid released reason=" << reason);
    setEnabled(false);
}

std::optional<double> MidLock::target() const {
    const auto& session = controller_.session();
    if (session.bookMidValid && session.bookMid > 0) return session.bookMid;
    if (session.lastTrade > 0) return session.lastTrade; // one-sided or withdrawn book
    return std::nullopt;
}

double MidLock::deadBand(double span) const {
    // Half a drawn device pixel: the camera's whole pixels per row at its tick
    // (hysteresis included). Before a camera exists, the stored window's scale.
    if (const auto cam = controller_.camera().restCameraNow(false); cam.valid && cam.rowPx > 0 && cam.tick > 0)
        return kDeadBandDevicePx * cam.tick / double(cam.rowPx);
    const auto size = environment_.surface();
    const double heightDev = size.heightLogical * size.dpr;
    return heightDev > 0 ? kDeadBandDevicePx * span / heightDev : 0.0;
}

void MidLock::targetChanged() {
    const auto* view = controller_.viewPtr();
    if (!enabled_ || moving_ || !view || !view->isTimeWindowValid() || !controller_.flags().priceKnown) return;
    const auto goal = target();
    if (!goal) return;
    const double lo = view->getMinPrice(), hi = view->getMaxPrice();
    if (std::abs(*goal - (lo + hi) * 0.5) <= deadBand(hi - lo)) return;
    wake();
}

bool MidLock::held() const {
    // A price glide or a gesture zooming price owns the drawn price; the lock takes
    // the window it leaves.
    const auto& glide = controller_.glide();
    return glide.price.active || (glide.gesture && glide.gesturePrice);
}

void MidLock::adoptStoredWindow() {
    const auto* view = controller_.viewPtr();
    const double lo = view->getMinPrice(), hi = view->getMaxPrice();
    if (lo == writtenLo_ && hi == writtenHi_) return;
    // Someone else moved price (a chart zoom's new span, a timeframe switch, a
    // symbol's carry): the spring continues from that window, from rest.
    centre_ = (lo + hi) * 0.5;
    span_ = hi - lo;
    velocity_ = 0;
    writtenLo_ = lo;
    writtenHi_ = hi;
}

void MidLock::advance(qint64 nowMs) {
    if (!moving_) return;
    QElapsedTimer cost;
    cost.start();
    const auto* view = controller_.viewPtr();
    const auto goal = target();
    if (!enabled_ || !view || !view->isTimeWindowValid() || !controller_.flags().priceKnown || !goal) {
        moving_ = false;
        return;
    }
    const qint64 dt = lastMs_ < 0 ? kFirstStepMs : std::clamp<qint64>(nowMs - lastMs_, 0, kMaxStepMs);
    lastMs_ = nowMs;
    if (held()) return;
    adoptStoredWindow();
    step(*goal, dt);
    write();
    sLog_Probe("midlock.advance", "us=" << cost.nsecsElapsed() / 1000.0 << " dt=" << dt << " centre=" << centre_
               << " target=" << *goal << " v=" << velocity_ << " moving=" << moving_);
}

void MidLock::step(double goal, qint64 dtMs) {
    const double eps = deadBand(span_);
    double x = centre_ - goal, v = velocity_;
    if (std::abs(x) > eps && dtMs > 0) {
        if (v * x > 0) v = 0; // moving away from a new target: retarget, no backward step
        // Exact critically damped step toward a constant target.
        const double w = kStiffnessPerSec, dt = double(dtMs) / 1000.0, e = std::exp(-w * dt);
        const double a = v + w * x;
        const double x1 = (x + a * dt) * e, v1 = (v - w * a * dt) * e;
        const bool crossed = x1 * x <= 0; // never past the target
        x = crossed ? 0.0 : x1;
        v = crossed ? 0.0 : v1;
    }
    if (std::abs(x) <= eps) { // rest on the target: the last step is under half a pixel
        x = 0;
        v = 0;
        moving_ = false;
    }
    centre_ = goal + x;
    velocity_ = v;
}

void MidLock::write() {
    auto* view = controller_.viewPtr();
    const double lo = centre_ - span_ * 0.5, hi = centre_ + span_ * 0.5;
    if (lo == view->getMinPrice() && hi == view->getMaxPrice()) return;
    writtenLo_ = lo; // the change this write emits is the lock's own (no wake)
    writtenHi_ = hi;
    controller_.setGpuViewportSelf(view->getVisibleTimeStart(), view->getVisibleTimeEnd(), lo, hi, true);
    writtenLo_ = view->getMinPrice(); // after any span clamp
    writtenHi_ = view->getMaxPrice();
}

} // namespace chart
