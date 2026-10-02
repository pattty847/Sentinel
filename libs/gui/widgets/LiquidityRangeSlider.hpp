#pragma once
// The GPU chart's liquidity range filter (S7b, owner decision 2): one two-handle
// slider on a log scale. The low handle hides cells below it (no colour, no
// label); the high handle is the colour saturation point. Values are base-asset
// quantities (the chart settings sensitivityMin/sensitivityMax).
//
// The ends span the liquidity of the current view (setDomain, from the label
// window), widened to always include both handles. While a handle is dragged the
// ends stay where they were at the press, so the handle never jumps under the
// cursor. GUI thread only.
#include <QWidget>

class LiquidityRangeSlider final : public QWidget {
    Q_OBJECT
public:
    static constexpr double kMinRatio = 1.05; // high >= low * kMinRatio
    explicit LiquidityRangeSlider(QWidget *parent = nullptr);

    // The liquidity in view (lo < hi, both > 0); an invalid pair keeps the ends
    // around the handles (a decade beyond each).
    void setDomain(double lo, double hi);
    // The handles (no signal). Invalid values are ignored.
    void setValues(double low, double high);
    double low() const { return low_; }
    double high() const { return high_; }
    // The ends drawn now (the domain widened to the handles).
    double endLo() const;
    double endHi() const;
    bool dragging() const { return active_ != Handle::None; }
    // Track geometry (tests): the x of a value and the value at an x.
    double xOf(double value) const;
    double valueAt(double x) const;

    QSize sizeHint() const override { return {170, 22}; }
    QSize minimumSizeHint() const override { return {90, 18}; }

signals:
    // A user edit. final is false while dragging and true on release.
    void rangeEdited(double low, double high, bool final);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum class Handle { None, Low, High };
    double trackLeft() const;
    double trackRight() const;
    void moveActive(double x);
    void refreshTip();

    double domainLo_ = 0, domainHi_ = 0;
    double low_ = 0.05, high_ = 50;
    Handle active_ = Handle::None;
    double frozenLo_ = 0, frozenHi_ = 0; // the ends at the press
};
