#pragma once

#include "chart/core/ChartCamera.hpp"
#include "chart/core/ChartSession.hpp"
#include "chart/core/ZoomGlide.hpp"
#include "render/GridViewState.hpp"

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

namespace chart {

struct PlacementFlags {
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

class ChartViewController : public QObject {
    Q_OBJECT
public:
    ChartViewController(IChartEnvironment& environment, IHeatmapViewSink& sink,
                        const ITickOracle& ticks, const PublishedFrame& published, QObject* parent = nullptr);

    GridViewState* viewPtr() const { return view_.get(); }
    ChartSession& session() { return session_; }
    const ChartSession& session() const { return session_; }
    PlacementFlags& flags() { return flags_; }
    const PlacementFlags& flags() const { return flags_; }
    InitialViewSettings& initialViewSettings() { return initialViewSettings_; }
    const InitialViewSettings& initialViewSettings() const { return initialViewSettings_; }

    ZoomGlideState& glide() { return glide_; }
    const ZoomGlideState& glide() const { return glide_; }
    ChartCamera& camera() { return camera_; }
    const ChartCamera& camera() const { return camera_; }

    ZoomGlide& zoomGlide() { return zoomGlide_; }
    void advanceFrame(qint64 now) { zoomGlide_.advanceZoomGlide(now); }

    void applyGpuLimits();
    void syncGpuView();
    void setGpuViewportSelf(qint64 start, qint64 end, double priceMin, double priceMax,
                            bool preservePlacement = false);
    double gpuLivePrice() const;

private:
    IChartEnvironment& environment_;
    IHeatmapViewSink& sink_;
    std::unique_ptr<GridViewState> view_;
    ChartSession session_;
    PlacementFlags flags_;
    InitialViewSettings initialViewSettings_;
    ZoomGlideState glide_;
    ChartCamera camera_;
    ZoomGlide zoomGlide_;
};

} // namespace chart
