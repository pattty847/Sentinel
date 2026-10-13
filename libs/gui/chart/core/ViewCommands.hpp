#pragma once
#include "chart/core/ChartPorts.hpp"
#include <QObject>
#include <QString>

namespace chart {
class ChartViewController;
struct ViewportRequest {
    std::optional<qint64> startMs, endMs;
    std::optional<double> priceMin, priceMax;
    std::optional<bool> followLive, autoScale;
    QString fit;
};

class ViewCommands : public QObject {
    Q_OBJECT
public:
    ViewCommands(ChartViewController& controller, IChartEnvironment& environment, ITickOracle& ticks);
    void setAutoPriceScale(bool enabled);
    bool fitView(bool time, bool price);
    QString applyViewportRequest(const ViewportRequest& request);
    bool resetView();
    void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    void resetZoom();
    void beginPanAt(double x, double y);
    void updatePanAt(double x, double y);
    void endPanAt();
    void panLeft();
    void panRight();
    void panUp();
    void panDown();
    void onViewChanged(qint64 startTimeMs, qint64 endTimeMs, double minPrice, double maxPrice);
    void onViewportChanged();
    void onHeatmapSnapshot();
    void onHeatmapLive();
    void refitAfterHeatmapLive();
    void onHeatmapTick();
    void onHeatmapLimits();
    void onAutoPriceScaleChanged();
    void clearAutoPriceTick();
signals:
    void heatmapTickSizeChanged();
    void viewportChanged();
    void autoPriceScaleChanged();
    void updateRequested();
    void liveRenderTick();
private:
    struct FitWindow { qint64 start, end; double lo, hi; };
    bool fitUnavailable(bool time, bool price) const;
    bool fitAxes(bool time, bool price, FitWindow& bounds);
    void anchorExplicitViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    struct RequestWindow {
        qint64 start0, end0, start, end;
        double lo, hi;
        bool follow;
    };
    bool resolveFlags(const ViewportRequest& request);
    RequestWindow resolveWindow(const ViewportRequest& request, qint64 start0, qint64 end0, bool follow);
    void commit(const RequestWindow& window);
    uint64_t livePriceFitRevision_ = 0;
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    ITickOracle& ticks_;
};
} // namespace chart
