#pragma once

#include "render/ChartRaster.hpp"
#include <functional>

class QObject;
class GridViewState;

namespace chart {

class IChartCameraSource {
public:
    virtual ~IChartCameraSource() = default;
    virtual chart_raster::RasterCamera rasterCameraNow(bool includeDrag = true) const = 0;
    virtual GridViewState* getViewState() const = 0;
    virtual double chartWidth() const = 0;
    virtual double chartHeight() const = 0;
    virtual void connectCameraChanged(QObject* context, std::function<void()> onChange) = 0;
};

} // namespace chart
