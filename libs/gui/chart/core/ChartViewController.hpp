#pragma once

#include "chart/core/ChartCamera.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/core/ViewCommands.hpp"
#include "chart/core/LivePlacement.hpp"
#include "chart/core/PriceFitter.hpp"
#include "chart/core/PriceCarryPolicy.hpp"
#include "chart/core/TimeframeSwitch.hpp"
#include "chart/core/ChartSession.hpp"
#include "chart/core/ZoomGlide.hpp"
#include "chart/core/ZoomController.hpp"
#include "render/GridViewState.hpp"

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

class QElapsedTimer;

namespace chart {

struct ViewSnapshot {
    qint64 start = 0, end = 0, timeframeMs = 0;
    double low = 0, high = 0, width = 0, height = 0;
    uint64_t viewportVersion = 0;
    bool timeWindowValid = false, followLive = false, autoScale = false;
};

class ChartViewController : public QObject {
    Q_OBJECT
public:
    ChartViewController(IChartEnvironment& environment, IHeatmapViewSink& sink,
                        ITickOracle& ticks, const PublishedFrame& published, QObject* parent = nullptr);

    ViewSnapshot snapshot() const;
    QString applyViewportRequest(const ViewportRequest& request) { return commands_.applyViewportRequest(request); }

    GridViewState* viewPtr() const { return view_.get(); }
    ChartSession& session() { return session_; }
    const ChartSession& session() const { return session_; }
    PlacementFlags& flags() { return livePlacement_.flags(); }
    const PlacementFlags& flags() const { return livePlacement_.flags(); }
    InitialViewSettings& initialViewSettings() { return livePlacement_.initialViewSettings(); }
    const InitialViewSettings& initialViewSettings() const { return livePlacement_.initialViewSettings(); }

    ZoomGlideState& glide() { return glide_; }
    const ZoomGlideState& glide() const { return glide_; }
    ChartCamera& camera() { return camera_; }
    const ChartCamera& camera() const { return camera_; }

    LivePlacement& livePlacement() { return livePlacement_; }

    PriceCarryPolicy& priceCarry() { return priceCarry_; }
    TimeframeSwitch& timeframeSwitch() { return timeframeSwitch_; }
    void setTimeframe(int next);

    PriceFitter& priceFitter() { return priceFitter_; }

    ViewCommands& commands() { return commands_; }

    ZoomController& zoom() { return zoom_; }

    ZoomGlide& zoomGlide() { return zoomGlide_; }
    void advanceFrame(qint64 now) { zoomGlide_.advanceZoomGlide(now); }

    FrameInputs frameInputs(const QRectF& bounds, double dpr, const QElapsedTimer& clock,
                            const TimeAuthority& time, FrameContext::OverlayActivationSet overlays) const;

    void applyGpuLimits();
    void syncGpuView();
    void setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax,
                            bool preservePlacement = false);
    double gpuLivePrice() const;

signals:
    void liveRenderTick();
    void timeframeChanged(int next);

private:
    IChartEnvironment& environment_;
    IHeatmapViewSink& sink_;
    std::unique_ptr<GridViewState> view_;
    ChartSession session_;
    ZoomGlideState glide_;
    ChartCamera camera_;
    ZoomGlide zoomGlide_;
    ZoomController zoom_;
    PriceFitter priceFitter_;
    LivePlacement livePlacement_;
    PriceCarryPolicy priceCarry_;
    TimeframeSwitch timeframeSwitch_;
    ViewCommands commands_;
};

} // namespace chart
