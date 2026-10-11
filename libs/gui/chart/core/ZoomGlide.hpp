#pragma once

#include "chart/core/ZoomGlideState.hpp"

#include <QObject>
#include <functional>
#include <utility>

namespace chart {

class ChartCamera;

class ZoomGlide : public QObject {
    Q_OBJECT
public:
    ZoomGlide(ZoomGlideState& state, const ChartCamera& camera);
    void setClock(std::function<qint64()> clock) { now_ = std::move(clock); }
    bool gliding() const { return state_.time.active || state_.price.active; }
    void endZoomGlide(bool time = true, bool price = true);
    void startAxisGlides(const chart_raster::RasterCamera& drawn, const chart_raster::RasterCamera& restBefore,
                         double anchorTime, double anchorPrice, bool forceTime, bool forcePrice);
    void advanceZoomGlide(qint64 now);
    void shiftBoth(double dx, double dy);
    void shiftTime(qint64 shift);

signals:
    void rasterChanged();
    void viewportChanged();
    void polishRequested();
    void updateRequested();

private:
    void advanceAxisGlide(AxisGlide& glide, bool time, const chart_raster::RasterCamera& rest,
                          const chart_raster::RasterCamera& intended, qint64 now);
    ZoomGlideState& state_;
    const ChartCamera& camera_;
    std::function<qint64()> now_ = [] { return 0; };
};

} // namespace chart
