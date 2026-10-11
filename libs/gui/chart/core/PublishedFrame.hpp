#pragma once

#include "render/ITimeAxisMappingProvider.hpp"

#include <mutex>

namespace chart {

class PublishedFrame {
public:
    MappingFrameContext current() const;
    chart_raster::RasterStep step() const;
    void publish(const MappingFrameContext& frame);
    void setStep(chart_raster::RasterStep step);

private:
    mutable std::mutex mutex_;
    MappingFrameContext last_;
    chart_raster::RasterStep step_;
};

} // namespace chart
