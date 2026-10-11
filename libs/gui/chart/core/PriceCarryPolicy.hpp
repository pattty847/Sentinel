#pragma once
#include "chart/core/ChartPorts.hpp"
#include <functional>
#include <memory>

namespace chart {
class ChartViewController;
class IPriceCarryTimer {
public:
    virtual ~IPriceCarryTimer() = default;
    virtual bool isActive() const = 0;
    virtual void start(int ms) = 0;
    virtual void stop() = 0;
};

class PriceCarryPolicy {
public:
    static constexpr int kCarryLiveOnlyWaitMs = 2000;
    using TimerFactory = std::function<std::unique_ptr<IPriceCarryTimer>(std::function<void()>)>;
    PriceCarryPolicy(ChartViewController& controller, IChartEnvironment& environment);
    void setTimerFactoryForTest(TimerFactory factory);
    void capture();
    bool apply(double now, bool liveOnly = false);
    void stop();
    void limitsChanged();
    void priceInteracted();
private:
    double capturePrice();
    void waitForScale();
    void timeout();
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    std::unique_ptr<IPriceCarryTimer> timer_;
    TimerFactory timerFactory_;
};
} // namespace chart
