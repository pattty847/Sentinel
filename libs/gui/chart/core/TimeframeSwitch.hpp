#pragma once
#include "chart/core/LivePlacement.hpp"
#include "render/ChartRaster.hpp"

namespace chart {
class ChartViewController;
class PublishedFrame;

struct TimeframeSwitchInputs {
    int64_t previousTfMs = 0, nextTfMs = 0, oldStart = 0, oldEnd = 0;
    int64_t previousLiveEndMs = 0, nextLiveEndMs = 0;
    bool following = false, keepColumns = false;
    double maxTimeSpanMs = 0;
    chart_raster::RasterCamera drawnBefore;
    std::function<chart_raster::RasterCamera(int64_t end, int64_t span)> restAt;
    std::function<int64_t(int64_t span)> nominalLiveEnd;
};
struct TimeframeSwitchPlan {
    bool apply = false;
    int64_t start = 0, end = 0;
    double anchorFracX = 1.0;
    std::optional<LivePlacement::FollowPadding> followPadding;
};
TimeframeSwitchPlan planTimeframeSwitch(const TimeframeSwitchInputs& in);

class TimeframeSwitch {
public:
    TimeframeSwitch(ChartViewController& controller, IChartEnvironment& environment, const PublishedFrame& published);
    void setHooks(std::function<void(int)> activeTimeframe, std::function<void(int)> layerTimeframe);
    bool request(int next) const;
    TimeframeSwitchInputs capture(int next);
    void updateLimits(const TimeframeSwitchInputs& in);
    TimeframeSwitchPlan plan(TimeframeSwitchInputs in) const;
    void apply(const TimeframeSwitchInputs& in, const TimeframeSwitchPlan& plan);
private:
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    const PublishedFrame& published_;
    std::function<void(int)> activeTimeframe_ = [](int) {};
    std::function<void(int)> layerTimeframe_ = [](int) {};
};
} // namespace chart
