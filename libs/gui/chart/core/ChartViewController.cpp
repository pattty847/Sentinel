#include "chart/core/ChartViewController.hpp"

namespace chart {

namespace {
struct LivePricePolicy {
    static double choose(const ChartSession& session, const IChartEnvironment& environment) {
        if (session.bookMid > 0) return session.bookMid;
        if (session.lastTrade > 0) return session.lastTrade;
        return environment.attached() ? environment.recentMidPrice() : 0.0;
    }
};
} // namespace

ChartViewController::ChartViewController(IChartEnvironment& environment, IHeatmapViewSink& sink, QObject* parent)
    : QObject(parent), environment_(environment), sink_(sink), view_(std::make_unique<GridViewState>(parent)) {}

void ChartViewController::applyGpuLimits() {
    if (!environment_.attached() || flags_.limitsDeferred) return;
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
    sink_.setView(view, flags_.priceKnown);
}

void ChartViewController::setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax,
                                              bool preservePlacement) {
    flags_.selfViewport = true;
    view_->setViewport(start, end, priceMin, priceMax, preservePlacement);
    flags_.selfViewport = false;
}

double ChartViewController::gpuLivePrice() const {
    return LivePricePolicy::choose(session_, environment_);
}

} // namespace chart
