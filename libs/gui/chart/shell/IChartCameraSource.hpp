#pragma once

#include "render/ChartRaster.hpp"

class GridViewState;

namespace chart {

class IChartCameraSource {
public:
    virtual ~IChartCameraSource() = default;
    virtual chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const = 0;
    virtual GridViewState* getViewState() const = 0;
    virtual double chartWidth() const = 0;
    virtual double chartHeight() const = 0;
};

} // namespace chart
