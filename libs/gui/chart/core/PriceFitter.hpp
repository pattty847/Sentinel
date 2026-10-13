#pragma once

#include "chart/core/ChartPorts.hpp"
#include <functional>

namespace chart {
class ChartViewController;

class PriceFitter {
public:
    static constexpr double kAutoScrollPaddingFrac = 0.08;
    static constexpr double kFitPriceMargin = 0.06;
    struct PriceCarry { double spanRatio = 0.0, heightFrac = 0.0; };

    PriceFitter(ChartViewController& controller, IChartEnvironment& environment, ITickOracle& ticks);
    void setCandleSource(const ICandleSource* source) { candles_ = source; }
    const ICandleSource* candleSource() const { return candles_; }
    void setRequestUpdate(std::function<void()> request) { requestUpdate_ = std::move(request); }
    std::optional<PriceCarry>& priceCarry() { return priceCarry_; }
    // Shared scratch for the placement policy's newest-close lookup.
    std::vector<CandleBar>& scratchBars() { return bars_; }

    qint64 gpuLiveEndMs(qint64 spanMs) const;
    std::optional<std::pair<qint64, qint64>> gpuFitTimeWindow() const;
    std::optional<std::pair<double, double>> gpuFitPriceWindow(qint64 start, qint64 end, bool candlesOnly,
                                                            std::optional<double> anchorFracX = std::nullopt);
    bool autoPriceFit(qint64 start, qint64 end, double& priceMin, double& priceMax);
    void refitAutoPrice();
    uint64_t autoPriceFitRevision() const { return autoPriceFitRevision_; }
    uint64_t autoPriceSolveCountForTest() const { return autoPriceSolveCount_; }
    void candlesDirty(const QString& symbol, int64_t timeframeSec, qint64 dirtyStart, qint64 dirtyEnd);

private:
    struct PriceRange { double lo = 0, hi = 0, centre = 0, span = 0; };
    struct VisibleBars { size_t used = 0; double newestClose = 0; };
    struct FitContext {
        qint64 start, end;
        double drawnStart, drawnEnd;
        bool onePrice;
        double current;
    };
    VisibleBars collectVisibleBars(int64_t tf, double drawnStart, double drawnEnd, double& lo, double& hi);
    bool centreAndSpan(PriceRange& range, size_t used, double current, bool candlesOnly) const;
    void applySpanLimits(PriceRange& range, size_t used, double newestClose) const;
    std::pair<double, double> snapToRows(PriceRange& range, const FitContext& context);
    std::pair<double, double> onePriceRows(double centre, double current) const;
    void solveAutoRows(PriceRange& range, const FitContext& context, int H, double tick);

    uint64_t autoPriceFitRevision_ = 0;
    uint64_t autoPriceSolveCount_ = 0;
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    ITickOracle& ticks_;
    const ICandleSource* candles_ = nullptr;
    std::vector<CandleBar> bars_;
    std::optional<PriceCarry> priceCarry_;
    std::function<void()> requestUpdate_ = [] {};
};
} // namespace chart
