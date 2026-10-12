#pragma once
#include "chart/core/ChartViewController.hpp"
#include <QObject>
namespace chart {
class ChartViewQml : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool autoScrollEnabled READ autoScrollEnabled WRITE enableAutoScroll NOTIFY autoScrollEnabledChanged)
    Q_PROPERTY(qint64 visibleTimeStart READ getVisibleTimeStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 visibleTimeEnd READ getVisibleTimeEnd NOTIFY viewportChanged)
    Q_PROPERTY(double minPrice READ getMinPrice NOTIFY viewportChanged)
    Q_PROPERTY(double maxPrice READ getMaxPrice NOTIFY viewportChanged)
    Q_PROPERTY(double heatmapTickSize READ heatmapTickSize NOTIFY heatmapTickSizeChanged)
    Q_PROPERTY(int timeframeMs READ getCurrentTimeframe WRITE setTimeframe NOTIFY timeframeChanged)
    Q_PROPERTY(QPointF panVisualOffset READ getPanVisualOffset NOTIFY panVisualOffsetChanged)
    Q_PROPERTY(bool autoPriceScale READ autoPriceScale WRITE setAutoPriceScale NOTIFY autoPriceScaleChanged)
    Q_PROPERTY(QObject* viewState READ viewState CONSTANT)
public:
    void bind(ChartViewController& controller, IChartEnvironment& environment) { controller_ = &controller; environment_ = &environment; }
    ChartViewController& controller() const { return *controller_; }
    GridViewState* getViewState() const { return controller_ ? controller_->viewPtr() : nullptr; }
    QObject* viewState() const { return getViewState(); }
    bool autoScrollEnabled() const { return getViewState() ? getViewState()->isAutoScrollEnabled() : false; }
    Q_INVOKABLE qint64 getVisibleTimeStart() const;
    Q_INVOKABLE qint64 getVisibleTimeEnd() const;
    Q_INVOKABLE double getMinPrice() const;
    Q_INVOKABLE double getMaxPrice() const;
    int getCurrentTimeframe() const { return static_cast<int>(controller_->session().timeframeMs); }
    Q_INVOKABLE QPointF getPanVisualOffset() const;
    double heatmapTickSize() const;
    bool autoPriceScale() const { return getViewState() && getViewState()->autoPriceScale(); }
    void setAutoPriceScale(bool enabled);
    Q_INVOKABLE bool fitView(bool time, bool price);
    Q_INVOKABLE bool fitTimeToData() { return fitView(true, false); }
    Q_INVOKABLE bool fitPriceToData() { return fitView(false, true); }
    Q_INVOKABLE bool resetView();
    Q_INVOKABLE void setViewport(qint64 timeStart, qint64 timeEnd, double priceMin, double priceMax);
    Q_INVOKABLE void setTimeframe(int timeframe_ms);
    // A click (mouse wheel
    // notch, keyboard, toolbar) moves to the next rung, a view with whole device pixels
    // per column and per row, by a kZoomGlideMs eased glide drawn with coverage; a
    // pinch or trackpad scroll zooms continuously and settles on the nearest rung.
    // Item coordinates (logical px); time/price select the axes.
    // The axes (QML). Unphased mouse notches are clicks (pixel deltas may be present); a
    // trackpad scroll is continuous until its ScrollEnd. Drags are continuous until the
    // release (endZoomGesture). A price zoom takes price over (auto price scale off).
    Q_INVOKABLE void zoomClicks(int clicks, double x, double y, bool time = true, bool price = true);
    Q_INVOKABLE void zoomContinuous(double factor, double x, double y, bool time = true, bool price = true);
    Q_INVOKABLE void endZoomGesture();
    Q_INVOKABLE void zoomTimeWheel(int angleDelta, int pixelDelta, int phase, double x);
    Q_INVOKABLE void zoomPriceWheel(int angleDelta, int pixelDelta, int phase, double y);
    Q_INVOKABLE void zoomTimeDrag(double delta, double x);
    Q_INVOKABLE void zoomPriceDrag(double delta, double y);
    Q_INVOKABLE void zoomTimeClicks(int clicks, double x);
    Q_INVOKABLE void zoomPriceClicks(int clicks, double y);
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void beginPanAt(double x, double y);
    Q_INVOKABLE void updatePanAt(double x, double y);
    Q_INVOKABLE void endPanAt();
    Q_INVOKABLE void panLeft();
    Q_INVOKABLE void panRight();
    Q_INVOKABLE void panUp();
    Q_INVOKABLE void panDown();
    Q_INVOKABLE void enableAutoScroll(bool enabled);
signals:
    void autoScrollEnabledChanged();
    void autoPriceScaleChanged();
    void viewportChanged();
    void timeframeChanged();
    void panVisualOffsetChanged();
    void heatmapTickSizeChanged();
private:
    double width() const { return environment_->surface().widthLogical; }
    double height() const { return environment_->surface().heightLogical; }
    ChartViewController* controller_ = nullptr;
    IChartEnvironment* environment_ = nullptr;
};
} // namespace chart
