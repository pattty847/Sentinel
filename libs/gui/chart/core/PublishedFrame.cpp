#include "PublishedFrame.hpp"

namespace chart {

MappingFrameContext PublishedFrame::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

chart_raster::RasterStep PublishedFrame::step() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return step_;
}

void PublishedFrame::publish(const MappingFrameContext& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_ = frame;
}

void PublishedFrame::setStep(chart_raster::RasterStep step) {
    std::lock_guard<std::mutex> lock(mutex_);
    step_ = step;
}

} // namespace chart
