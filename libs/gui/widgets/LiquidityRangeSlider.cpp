#include "LiquidityRangeSlider.hpp"
#include "SentinelLogging.hpp"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kHandleRadius = 6;
bool positive(double v) { return std::isfinite(v) && v > 0; }
QString amount(double v) { return QString::number(v, 'g', 3); }
} // namespace

LiquidityRangeSlider::LiquidityRangeSlider(QWidget *parent) : QWidget(parent) {
    setObjectName("liquidityRangeSlider");
    setMouseTracking(false);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    refreshTip();
}

void LiquidityRangeSlider::setDomain(double lo, double hi) {
    if (!positive(lo) || !positive(hi) || !(hi > lo)) lo = hi = 0;
    if (lo == domainLo_ && hi == domainHi_) return;
    domainLo_ = lo;
    domainHi_ = hi;
    update();
}

void LiquidityRangeSlider::setValues(double low, double high) {
    if (!positive(low) || !positive(high) || !(high > low)) return;
    if (low == low_ && high == high_) return;
    low_ = low;
    high_ = high;
    refreshTip();
    update();
}

double LiquidityRangeSlider::endLo() const {
    if (dragging()) return frozenLo_;
    const double lo = domainLo_ > 0 ? domainLo_ : low_ / 10;
    return std::min(lo, low_ / 1.25);
}

double LiquidityRangeSlider::endHi() const {
    if (dragging()) return frozenHi_;
    const double hi = domainHi_ > 0 ? domainHi_ : high_ * 10;
    return std::max(hi, high_ * 1.25);
}

double LiquidityRangeSlider::trackLeft() const { return kHandleRadius + 1; }
double LiquidityRangeSlider::trackRight() const { return std::max(trackLeft() + 1, width() - kHandleRadius - 1); }

double LiquidityRangeSlider::xOf(double value) const {
    const double lo = std::log(endLo()), hi = std::log(endHi());
    const double t = hi > lo ? (std::log(std::max(value, 1e-300)) - lo) / (hi - lo) : 0.5;
    return trackLeft() + std::clamp(t, 0.0, 1.0) * (trackRight() - trackLeft());
}

double LiquidityRangeSlider::valueAt(double x) const {
    const double t = std::clamp((x - trackLeft()) / (trackRight() - trackLeft()), 0.0, 1.0);
    const double lo = std::log(endLo()), hi = std::log(endHi());
    return std::exp(lo + t * (hi - lo));
}

void LiquidityRangeSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double cy = height() / 2.0;
    const double l = trackLeft(), r = trackRight(), xl = xOf(low_), xh = xOf(high_);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x3A, 0x41, 0x4B)); // hidden below the low handle
    p.drawRoundedRect(QRectF(l, cy - 2, r - l, 4), 2, 2);
    QLinearGradient ramp(xl, 0, xh, 0); // colour ramp between the handles
    ramp.setColorAt(0, QColor(0x1F, 0x4E, 0x6E));
    ramp.setColorAt(1, QColor(0x4F, 0xD8, 0xFF));
    p.setBrush(ramp);
    p.drawRect(QRectF(xl, cy - 2, xh - xl, 4));
    p.setBrush(QColor(0x4F, 0xD8, 0xFF)); // saturated above the high handle
    p.drawRect(QRectF(xh, cy - 2, r - xh, 4));
    for (const auto &[x, handle] : {std::pair{xl, Handle::Low}, std::pair{xh, Handle::High}}) {
        p.setPen(QPen(QColor(0x0E, 0x11, 0x16), 1.5));
        p.setBrush(active_ == handle ? QColor(0xFF, 0xFF, 0xFF) : QColor(0xC9, 0xD3, 0xDE));
        p.drawEllipse(QPointF(x, cy), kHandleRadius, kHandleRadius);
    }
}

void LiquidityRangeSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    const double x = event->position().x();
    frozenLo_ = endLo();
    frozenHi_ = endHi();
    const double dl = std::abs(x - xOf(low_)), dh = std::abs(x - xOf(high_));
    active_ = dl < dh || (dl == dh && x < xOf(low_)) ? Handle::Low : Handle::High;
    moveActive(x);
    event->accept();
}

void LiquidityRangeSlider::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging()) return QWidget::mouseMoveEvent(event);
    moveActive(event->position().x());
    event->accept();
}

void LiquidityRangeSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging()) return QWidget::mouseReleaseEvent(event);
    moveActive(event->position().x());
    active_ = Handle::None;
    sLog_App("ui: liquidity range low=" << low_ << " high=" << high_);
    emit rangeEdited(low_, high_, true);
    update();
    event->accept();
}

void LiquidityRangeSlider::moveActive(double x) {
    const double v = valueAt(x);
    double low = low_, high = high_;
    if (active_ == Handle::Low) low = std::min(v, high_ / kMinRatio);
    else if (active_ == Handle::High) high = std::max(v, low_ * kMinRatio);
    // Three significant digits: settings and tooltips stay readable.
    auto round3 = [](double value) { return QString::number(value, 'g', 3).toDouble(); };
    low = round3(low);
    high = round3(high);
    if (!(high > low)) return;
    const bool changed = low != low_ || high != high_;
    low_ = low;
    high_ = high;
    refreshTip();
    update();
    if (changed) emit rangeEdited(low_, high_, false);
}

void LiquidityRangeSlider::refreshTip() {
    setToolTip(QStringLiteral("Liquidity range (log scale, base-asset size per cell)\n"
                              "Low %1: smaller cells get no colour and no label\n"
                              "High %2: colour saturates at and above it")
                   .arg(amount(low_), amount(high_)));
}
