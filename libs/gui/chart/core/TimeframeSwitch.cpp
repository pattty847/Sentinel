#include "chart/core/TimeframeSwitch.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/FrameContextBuilder.hpp"
#include "servermodel/RecordingCodec.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>

namespace chart {
namespace {
double nowCentre(int64_t anchor, int64_t tf) {
    return static_cast<double>(recording::floorDiv(anchor + tf - 1, tf) * tf) - static_cast<double>(tf) * 0.5;
}
std::optional<double> rightFraction(const TimeframeSwitchInputs& in) {
    if (in.liveAnchorMs > 0 && in.oldEnd > in.oldStart && !in.following) {
        const double now = nowCentre(in.liveAnchorMs, in.previousTfMs);
        if (in.drawnBefore.valid) {
            const double x = in.drawnBefore.xDev(now) / in.drawnBefore.widthDev;
            if (x >= 0.0 && x <= 1.0) return 1.0 - x;
        } else if (now >= double(in.oldStart) && now <= double(in.oldEnd)) {
            return (static_cast<double>(in.oldEnd) - now) / static_cast<double>(in.oldEnd - in.oldStart);
        }
    }
    return std::nullopt;
}
std::optional<LivePlacement::FollowPadding> followPadding(const TimeframeSwitchInputs& in, int64_t nominal,
                                                        int64_t span) {
    const auto& drawnBefore = in.drawnBefore;
    const int64_t anchor = in.liveAnchorMs, previousTf = in.previousTfMs, tf = in.nextTfMs;
    if (!drawnBefore.valid || anchor <= 0 || nominal <= 0) return std::nullopt;
    const auto dest = in.restAt(nominal, span);
    // A capped column width re-lays out at the nominal live end.
    if (!dest.valid || dest.colPx != drawnBefore.colPx) return std::nullopt;
    const double oldLiveEnd = double(recording::floorDiv(anchor + previousTf - 1, previousTf) * previousTf);
    const double liveEndX = drawnBefore.xDev(oldLiveEnd);
    const int64_t newLiveEnd = recording::floorDiv(anchor + tf - 1, tf) * tf;
    const double C = dest.colPx, W = dest.widthDev;
    int64_t e = newLiveEnd + std::llround((W - liveEndX) * double(tf) / C);
    for (const int64_t candidate : {e, e - 1, e + 1})
        if (const auto cam = in.restAt(candidate, span); cam.valid && cam.colPx == dest.colPx &&
                                                       cam.xDev(double(newLiveEnd)) == liveEndX) {
            e = candidate;
            break;
        }
    return LivePlacement::FollowPadding{e - newLiveEnd, e - span, e, tf};
}
} // namespace

TimeframeSwitchPlan planTimeframeSwitch(const TimeframeSwitchInputs& in) {
    TimeframeSwitchPlan out;
    if (in.previousTfMs == in.nextTfMs || !in.keepColumns) return out;
    const double scaledSpan = static_cast<double>(in.oldEnd - in.oldStart) * static_cast<double>(in.nextTfMs) /
                              static_cast<double>(in.previousTfMs);
    const auto rightFrac = rightFraction(in);
    int64_t span = std::max<int64_t>(1, static_cast<int64_t>(std::llround(scaledSpan)));
    if (in.maxTimeSpanMs > 0)
        span = std::min(span, std::max<int64_t>(1, static_cast<int64_t>(std::floor(in.maxTimeSpanMs))));
    int64_t end = in.oldEnd;
    double anchorX = 1.0;
    if (in.following) {
        const int64_t nominal = in.nominalLiveEnd(span);
        if (nominal > 0) end = nominal;
        out.followPadding = followPadding(in, nominal, span);
        if (out.followPadding) end = out.followPadding->endMs;
    } else if (rightFrac) {
        end = static_cast<int64_t>(std::llround(nowCentre(in.liveAnchorMs, in.nextTfMs) + *rightFrac * double(span)));
        anchorX = 1.0 - *rightFrac;
    }
    out.apply = true;
    out.start = end - span;
    out.end = end;
    out.anchorFracX = anchorX;
    return out;
}

TimeframeSwitch::TimeframeSwitch(ChartViewController& controller, IChartEnvironment& environment,
                                 const PublishedFrame& published)
    : controller_(controller), environment_(environment), published_(published) {}

void TimeframeSwitch::setHooks(std::function<void(int)> activeTimeframe, std::function<void(int)> layerTimeframe) {
    activeTimeframe_ = std::move(activeTimeframe);
    layerTimeframe_ = std::move(layerTimeframe);
}

bool TimeframeSwitch::request(int next) const {
    if (controller_.session().timeframeMs == next) return false;
    sLog_Render("timeframe changed: prevMs=" << controller_.session().timeframeMs << " tfMs=" << next);
    return true;
}

TimeframeSwitchInputs TimeframeSwitch::capture(int next) {
    TimeframeSwitchInputs in;
    in.drawnBefore = controller_.camera().rasterCameraNow(false);
    in.previousTfMs = controller_.session().timeframeMs;
    in.nextTfMs = next;
    controller_.session().timeframeMs = next;
    if (next > 0) activeTimeframe_(next);
    auto* view = controller_.viewPtr();
    // Read the span BEFORE the new limits can clamp it. The 100 ms startup default
    // does not yet describe the number of columns the user chose.
    in.keepColumns = environment_.attached() && next > 0 && in.previousTfMs >= 1000 && view && view->isTimeWindowValid();
    in.oldStart = in.keepColumns ? view->getVisibleTimeStart() : 0;
    in.oldEnd = in.keepColumns ? view->getVisibleTimeEnd() : 0;
    in.liveAnchorMs = in.keepColumns ? environment_.liveAnchorMs() : 0;
    in.following = view && view->isAutoScrollEnabled();
    return in;
}

void TimeframeSwitch::updateLimits(const TimeframeSwitchInputs& in) {
    if (environment_.attached() && in.nextTfMs > 0) {
        controller_.flags().limitsDeferred = in.keepColumns;
        layerTimeframe_(static_cast<int>(in.nextTfMs));
        controller_.flags().limitsDeferred = false;
    }
}

TimeframeSwitchPlan TimeframeSwitch::plan(TimeframeSwitchInputs in) const {
    if (!in.keepColumns) return {};
    in.maxTimeSpanMs = environment_.limits().maxTimeMs;
    const auto surface = environment_.surface();
    const auto previous = published_.step();
    auto vp = FrameContextBuilder::viewportSnapshot(controller_.viewPtr());
    vp.dragging = false;
    vp.anchorFracX = 1.0;
    const auto raster = controller_.camera().rasterInputs(vp, environment_.drawnTick(), surface);
    in.restAt = [raster, previous](int64_t end, int64_t span) {
        auto probe = raster;
        probe.timeStart = end - span;
        probe.timeEnd = end;
        return chart_raster::computeRaster(probe, previous);
    };
    in.nominalLiveEnd = [this](int64_t span) { return controller_.priceFitter().gpuLiveEndMs(span); };
    return planTimeframeSwitch(in);
}

void TimeframeSwitch::apply(const TimeframeSwitchInputs& in, const TimeframeSwitchPlan& plan) {
    auto* view = controller_.viewPtr();
    auto& padding = controller_.livePlacement().followPadding();
    if (in.following) padding = plan.followPadding;
    const auto limits = environment_.limits();
    view->setMinSpans(limits.minTimeMs, limits.minPrice);
    view->setRasterAnchor(plan.anchorFracX, view->rasterAnchor().fracY);
    view->setViewportAndMaxSpans(plan.start, plan.end, view->getMinPrice(), view->getMaxPrice(),
                                 limits.maxTimeMs, limits.maxPrice);
    if (in.following && padding) padding->placement = view->placementVersion();
}
} // namespace chart
