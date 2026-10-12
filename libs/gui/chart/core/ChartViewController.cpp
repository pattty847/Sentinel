#include "chart/core/ChartViewController.hpp"
#include "render/FrameContextBuilder.hpp"

namespace chart {

FrameInputs ChartViewController::frameInputs(const QRectF& bounds, double dpr, const QElapsedTimer& clock,
                                           const TimeAuthority& time, FrameContext::OverlayActivationSet overlays) const {
    FrameInputs inputs;
    inputs.context = FrameContextBuilder::build(bounds, dpr, clock, time, view_.get(),
                                                overlays.heatmap, overlays.footprint, overlays.tpo);
    inputs.context.overlays.volumeProfile = overlays.volumeProfile;
    inputs.camera.raster = camera_.rasterInputs(inputs.context.viewport, 0, {bounds.width(), bounds.height(), dpr});
    inputs.camera.glide = glide_;
    inputs.timeframeMs = session_.timeframeMs;
    return inputs;
}


namespace {
struct LivePricePolicy {
    static double choose(const ChartSession& session, const IChartEnvironment& environment) {
        if (session.bookMid > 0) return session.bookMid;
        if (session.lastTrade > 0) return session.lastTrade;
        return environment.attached() ? environment.recentMidPrice() : 0.0;
    }
};
} // namespace

ChartViewController::ChartViewController(IChartEnvironment& environment, IHeatmapViewSink& sink,
                                         ITickOracle& ticks, const PublishedFrame& published, QObject* parent)
    : QObject(parent), environment_(environment), sink_(sink), view_(std::make_unique<GridViewState>(parent)),
      camera_(*view_, session_, environment, ticks, published, glide_), zoomGlide_(glide_, camera_), zoom_(*this, environment),
      priceFitter_(*this, environment, ticks), livePlacement_(*this, environment),
      priceCarry_(*this, environment), timeframeSwitch_(*this, environment, published), commands_(*this, environment, ticks) {
    livePlacement_.setApplyPriceCarry([this](double mid) { return priceCarry_.apply(mid); });
}

void ChartViewController::setTimeframe(int next) {
    if (!timeframeSwitch_.request(next)) return;
    zoomGlide_.land();
    const auto inputs = timeframeSwitch_.capture(next);
    timeframeSwitch_.updateLimits(inputs);
    const auto plan = timeframeSwitch_.plan(inputs);
    if (plan.apply) {
        timeframeSwitch_.apply(inputs, plan);
        syncGpuView();
        if (view_->isAutoScrollEnabled()) emit liveRenderTick();
    }
    emit timeframeChanged(next);
}

void ChartViewController::applyGpuLimits() {
    if (!environment_.attached() || flags().limitsDeferred) return;
    const auto limits = environment_.limits();
    view_->setMinSpans(limits.minTimeMs, limits.minPrice);
    view_->setMaxSpans(limits.maxTimeMs, limits.maxPrice);
}

void ChartViewController::syncGpuView() {
    if (!environment_.attached() || !view_->isTimeWindowValid()) return;
    heatmap::gpu::ViewWindow view{static_cast<double>(view_->getVisibleTimeStart()),
                                  static_cast<double>(view_->getVisibleTimeEnd()), view_->getMinPrice(),
                                  view_->getMaxPrice()};
    if (const auto rest = environment_.restView()) view = *rest;
    sink_.setView(view, flags().priceKnown);
}

void ChartViewController::setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax,
                                              bool preservePlacement) {
    flags().selfViewport = true;
    view_->setViewport(start, end, priceMin, priceMax, preservePlacement);
    flags().selfViewport = false;
}

double ChartViewController::gpuLivePrice() const {
    return LivePricePolicy::choose(session_, environment_);
}

} // namespace chart
