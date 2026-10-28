#pragma once
#include <QObject>
#include <optional>

namespace chart {
class ChartViewController;
class IChartEnvironment;

// Lock to mid: the price window stays centred on the book mid (the last trade while
// the book is one-sided or withdrawn) and glides to it each frame. It replaces the
// auto price scale (turning either on turns the other off); time follows live as
// before. The spring moves the committed window, so the drawn camera (device-pixel
// origin) moves by whole device pixels; it never steps away from its target and
// rests within half a device pixel of it.
//
// Routes (owner 2026-10-28): a chart drag pans time only (GridViewState keeps dy at
// 0 while locked); a chart wheel or pinch zooms price about the locked centre and
// the lock takes the new span; price-axis routes, keyboard price pans, explicit
// price bounds, price fits and the auto price scale release the lock.
class MidLock : public QObject {
    Q_OBJECT
public:
    // Critically damped spring, angular frequency per second: about 0.3 s to cover
    // 90% of a step (the owner A/Bs the feel).
    static constexpr double kStiffnessPerSec = 14.0;
    static constexpr double kDeadBandDevicePx = 0.5; // of the drawn camera's device pixels
    static constexpr qint64 kFirstStepMs = 16;  // a step that starts the spring from rest
    static constexpr qint64 kMaxStepMs = 50;    // a stalled frame does not jump the picture

    MidLock(ChartViewController& controller, IChartEnvironment& environment);
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    void release(const char* reason);
    // True while the spring needs frames (the shell's polish loop runs).
    bool moving() const { return moving_; }
    // A book top or trade arrived: wakes the spring when the target left the dead band.
    void targetChanged();
    void advance(qint64 nowMs);
    std::optional<double> target() const;
    double centre() const { return centre_; }
    double spanPrice() const { return span_; }
    double velocity() const { return velocity_; }
    // A chart zoom of price while locked anchors at the locked centre, not the cursor.
    double zoomAnchorY(double y, double height, bool price) const { return enabled_ && price ? height * 0.5 : y; }

signals:
    void enabledChanged();
    void polishRequested();

private:
    void wake();
    bool held() const;
    void adoptStoredWindow();
    double deadBand(double span) const;
    void step(double target, qint64 dtMs);
    void write();
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    bool enabled_ = false, moving_ = false;
    double centre_ = 0, span_ = 0, velocity_ = 0;
    double writtenLo_ = 0, writtenHi_ = 0; // the stored window after the lock's last write
    qint64 lastMs_ = -1;
};
} // namespace chart
