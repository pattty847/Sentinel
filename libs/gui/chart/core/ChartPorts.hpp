#pragma once

#include "datasources/CandleSeriesBuffer.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "render/heatmap/ViewWindow.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace chart {

struct SurfaceFacts {
    double widthLogical = 0;
    double heightLogical = 0;
    double dpr = 1;
};

struct SpanLimits {
    double minTimeMs = 0;
    double maxTimeMs = 0;
    double minPrice = 0;
    double maxPrice = 0;
};

class IChartEnvironment {
public:
    virtual ~IChartEnvironment() = default;
    virtual bool attached() const = 0;
    virtual SurfaceFacts surface() const = 0;
    virtual double drawnTick() const = 0;
    virtual double priceScale() const = 0;
    virtual bool manualTick() const = 0;
    virtual bool hasCurrentResolution() const = 0;
    virtual bool priceScaleCurrent() const = 0;
    virtual SpanLimits limits() const = 0;
    virtual int64_t liveAnchorMs() const = 0;
    virtual int64_t liveOpenEndMs() const = 0;
    virtual int64_t oldestAvailableMs() const = 0;
    virtual double recentMidPrice() const = 0;
    virtual std::optional<heatmap::gpu::ViewWindow> restView() const = 0;
};

class ITickOracle {
public:
    virtual ~ITickOracle() = default;
    virtual int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const = 0;
    virtual bool buildsTick(int64_t units, const heatmap::gpu::ViewWindow&) const = 0;
    virtual int64_t autoTickState() const = 0;
    virtual heatmap::AutoTickParams autoTickParams() const = 0;
    virtual bool setAutoPriceTick(std::optional<int64_t> units) = 0;
};

class IHeatmapViewSink {
public:
    virtual ~IHeatmapViewSink() = default;
    virtual void setView(const heatmap::gpu::ViewWindow& drawn, bool priceKnown) = 0;
};

using CandleBar = CandleSeriesBuffer::CandleBar;

class ICandleSource {
public:
    virtual ~ICandleSource() = default;
    virtual bool available() const { return true; }
    virtual void visibleBars(const QString& symbol, int64_t tfSec, int64_t start, int64_t end,
                             std::vector<CandleBar>& out) const = 0;
    virtual int64_t oldestTimeMs(const QString& symbol, int64_t tfSec) const = 0;
};

} // namespace chart
