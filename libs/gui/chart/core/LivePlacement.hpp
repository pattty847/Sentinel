#pragma once
#include "chart/core/ChartPorts.hpp"
#include <QObject>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace chart {
class ChartViewController;

struct PlacementFlags {
    void markPriceKnown() { priceKnown = true; reseedPrice = false; }
    bool priceKnown = false;
    bool reseedPrice = false;
    bool pristine = false;
    bool selfViewport = false;
    bool limitsDeferred = false;
};
struct InitialViewSettings {
    int columnPx = 8;
    int pricePct = 5;
};

class ILivePlacementTimer {
public:
    virtual ~ILivePlacementTimer() = default;
    virtual void setInterval(int ms) = 0;
    virtual bool isActive() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

class LivePlacement : public QObject {
    Q_OBJECT
public:
    // A timeframe switch preserves this padding while its placement is still current.
    struct FollowPadding {
        int64_t padMs = 0, startMs = 0, endMs = 0, tfMs = 0;
        uint64_t placement = 0;
    };
    using TimerFactory = std::function<std::unique_ptr<ILivePlacementTimer>(std::function<void()>)>;
    LivePlacement(ChartViewController& controller, IChartEnvironment& environment);
    PlacementFlags& flags() { return flags_; }
    const PlacementFlags& flags() const { return flags_; }
    InitialViewSettings& initialViewSettings() { return initialViewSettings_; }
    const InitialViewSettings& initialViewSettings() const { return initialViewSettings_; }
    std::optional<FollowPadding>& followPadding() { return followPadding_; }
    void setApplyPriceCarry(std::function<bool(double)> apply) { applyPriceCarry_ = std::move(apply); }
    void setTimingForTest(std::function<qint64()> clock, TimerFactory timerFactory);
    void stopBootstrap();
    void followGpuLive();
    void followGpuLiveTo(int64_t openEnd);
    void seedGpuViewport(double bestBid, double bestAsk);
    void returnGpuToLive();
    void bootstrapGpuTimeView();
    qint64 gpuInitialSpanMs(double widthPx) const;
    void enableAutoScroll(bool enabled);
    void setLiveBookTop(double bestBid, double bestAsk);
    static bool matchesUtf8Product(QStringView symbol, std::string_view product);
    void onTrade(const std::string& product, double price, int64_t timeMs);
    void resize(double width);
    static int64_t liveEndCeil(int64_t x, int64_t tf);
signals:
    void liveRenderTick();
    void autoScrollEnabledChanged();
    void updateRequested();
private:
    int64_t initialLiveEndMs() const;
    bool acceptTradeTime(int64_t timeMs);
    bool keepsFollowPadding(int64_t start, int64_t end, int64_t tf) const;
    bool shouldSeed(double bestBid, double bestAsk) const;
    static constexpr double kAutoScrollPaddingFrac = 0.08;
    static constexpr double kSeedBandTick = 1.0;
    static constexpr int kSeedBandRows = 2048;
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    PlacementFlags flags_;
    InitialViewSettings initialViewSettings_;
    std::optional<FollowPadding> followPadding_;
    std::unique_ptr<ILivePlacementTimer> bootstrapTimer_;
    std::function<qint64()> clock_;
    TimerFactory timerFactory_;
    std::function<bool(double)> applyPriceCarry_ = [](double) { return false; };
};
} // namespace chart
