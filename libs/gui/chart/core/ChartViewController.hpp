#pragma once

#include "chart/core/ChartPorts.hpp"
#include "render/GridViewState.hpp"

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

namespace chart {

struct ChartSession {
    QString symbol;
    int64_t timeframeMs = 100;
    double bookMid = 0.0;
    double lastTrade = 0.0;
};

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
    ChartViewController(IChartEnvironment& environment, IHeatmapViewSink& sink, QObject* parent = nullptr);

    GridViewState* viewPtr() const { return view_.get(); }
    ChartSession& session() { return session_; }
    const ChartSession& session() const { return session_; }
    PlacementFlags& flags() { return flags_; }
    const PlacementFlags& flags() const { return flags_; }
    InitialViewSettings& initialViewSettings() { return initialViewSettings_; }
    const InitialViewSettings& initialViewSettings() const { return initialViewSettings_; }

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
};

} // namespace chart
