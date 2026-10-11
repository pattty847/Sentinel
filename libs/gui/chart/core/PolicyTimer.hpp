#pragma once
#include <QTimer>
#include <functional>
#include <utility>

namespace chart {
// A single-shot QtCore timer behind a policy's injectable timer interface.
template <typename Interface>
class PolicyTimer final : public Interface {
public:
    explicit PolicyTimer(std::function<void()> timeout) {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, &timer_, std::move(timeout));
    }
    bool isActive() const override { return timer_.isActive(); }
    void start(int ms) override { timer_.start(ms); }
    void stop() override { timer_.stop(); }
private:
    QTimer timer_;
};
} // namespace chart
