#pragma once
#include "chart/core/ChartPorts.hpp"
#include "chart/core/WheelInterpreter.hpp"
#include "render/ChartRaster.hpp"
#include <QObject>
#include <functional>
#include <memory>
#include <utility>

class GridViewState;
namespace chart {
class ChartViewController;
class IZoomTimer {
public:
    virtual ~IZoomTimer() = default;
    virtual bool isActive() const = 0;
    virtual void start(int ms) = 0;
    virtual void stop() = 0;
};

class ZoomController : public QObject {
    Q_OBJECT
public:
    // Trackpad pause, including scrolls without phases or fingers resting on the pad.
    static constexpr int kGestureSettleMs = 160;
    // A lost explicit end (pinch or axis drag) must also settle.
    static constexpr int kGestureSafetyMs = 1000;
    enum class WheelRoute { Chart, TimeAxis, PriceAxis };
    using TimerFactory = std::function<std::unique_ptr<IZoomTimer>(std::function<void()>)>;
    ZoomController(ChartViewController& controller, IChartEnvironment& environment);
    void setTimerFactoryForTest(TimerFactory factory);
    void stop();
    bool gestureSettlePending() const;
    bool setZoomRung(const chart_raster::RasterCamera& drawn, double fracX, double fracY,
                     bool time, int colPx, bool price, chart_raster::RowRung row);
    void zoomStoredView(double factor, double x, double y, bool time, bool price);
    void zoomClicks(int clicks, double x, double y, bool time = true, bool price = true);
    void zoomContinuous(double factor, double x, double y, bool time = true, bool price = true);
    void zoomContinuousFor(double factor, double x, double y, bool time, bool price, int settleMs);
    void endZoomGesture();
    void wheelZoom(int angle, bool pixelDeltaNull, int phase, bool precise,
                   double x, double y, bool time, bool price, WheelRoute route);
    void zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x);
    void zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y);
    void zoomTimeDrag(double delta, double x);
    void zoomPriceDrag(double delta, double y);
    void zoomTimeClicks(int clicks, double x);
    void zoomPriceClicks(int clicks, double y);

signals:
    void updateRequested();

private:
    struct Axes { bool time, price; };
    bool canZoomContinuous(double factor) const;
    void applyContinuousView(const chart_raster::RasterCamera& cam, double factor,
                              double fracX, double fracY, Axes axes, Axes joins);
    struct ClickTarget {
        double fracX, fracY;
        int baseCol;
        chart_raster::RowRung baseRow;
        int col;
        chart_raster::RowRung row;
        double at, ap;
        bool zoomTime, zoomPrice;
    };
    GridViewState* getViewState() const;
    double width() const;
    double height() const;
    std::pair<double, double> zoomFrac(double x, double y) const;
    std::pair<int, int> columnLimits(double W, double tf) const;
    static double clampSpan(double span, double current, double maximum, double minimum);
    static double clampSpan(double span, double current, double maximum, double minimum, int clicks);
    void postZoom(bool price);
    chart_raster::RowRung clickPriceFallback(chart_raster::RowRung row,
                                            chart_raster::RowRung baseRow, int clicks, bool price) const;
    ClickTarget clickTarget(const chart_raster::RasterCamera& drawn, int clicks,
                            QPointF point, bool time, bool price) const;
    ChartViewController& controller_;
    IChartEnvironment& environment_;
    QPointF gestureAt_;
    std::unique_ptr<IZoomTimer> settleTimer_;
    TimerFactory timerFactory_;
    // Only the partial notch, for one route and its axes; reversal drops it.
    int wheelAngleRemainder_ = 0;
    int wheelCarryKey_ = -1;
};
} // namespace chart
