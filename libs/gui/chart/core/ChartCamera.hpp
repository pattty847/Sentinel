#pragma once

#include "chart/core/ChartPorts.hpp"
#include "chart/core/ZoomGlideState.hpp"
#include "render/FrameContext.hpp"

class GridViewState;

namespace chart {

struct ChartSession;
class PublishedFrame;

struct BinView {
    double timeLo = 0, timeHi = 0, priceLo = 0, priceHi = 0;
};

struct Cameras {
    chart_raster::RasterCamera rest, drawn;
    std::optional<BinView> binView;
};

// Captured values only: camera composition never follows GUI-owned references.
struct CameraInputs {
    chart_raster::RasterInputs raster;
    chart_raster::RasterStep previous;
    ZoomGlideState glide;
};

Cameras camerasFor(const CameraInputs& camera);
chart_raster::RasterCamera glidePart(const chart_raster::RasterCamera& from, const chart_raster::RasterCamera& end,
                                     double anchorTime, double anchorPrice, double progress, bool holding);

class ChartCamera {
public:
    ChartCamera(const GridViewState& view, const ChartSession& session, const IChartEnvironment& environment,
                const ITickOracle& ticks, const PublishedFrame& published, const ZoomGlideState& glide);
    chart_raster::RasterInputs rasterInputs(const FrameViewportSnapshot& viewport, double tick,
                                           SurfaceFacts surface) const;
    chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const;
    chart_raster::RasterCamera restCameraNow(bool includeDrag = true) const;
    chart_raster::RasterCamera intendedEndNow() const;
    std::pair<double, double> drawnTimeWindow(qint64 start, qint64 end,
                                            std::optional<double> anchorFracX = std::nullopt) const;
    bool panShift(QPointF drag, qint64& timeShiftMs, double& priceShift) const;
    chart_raster::TickAt tickPredictor(qint64 start, qint64 end, double anchorPrice, double fracY) const;

private:
    const GridViewState& view_;
    const ChartSession& session_;
    const IChartEnvironment& environment_;
    const ITickOracle& ticks_;
    const PublishedFrame& published_;
    const ZoomGlideState& glide_;
};

} // namespace chart
