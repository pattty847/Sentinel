#pragma once
#include "chart/feeds/FlowBufferAccess.hpp"
#include "chart/core/FootprintLod.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
#include <QTimer>
#include <array>
#include <functional>
#include <memory>
#include <mutex>

class IGridDataSource;
namespace chart {
class PublishedFrame;
class ChartViewController;
class LayerVisibility;
struct FootprintPresentation;
struct FlowCell {
    int64_t row = 0, span = 1; // [row, row + span) drawn ticks; coarse native bins stay intact
    int64_t bidVol = 0, askVol = 0;
};
struct FlowColumn {
    int64_t startMs = 0, baseScale = 1;
    int64_t bidVol = 0, askVol = 0, delta = 0; // whole-bar totals, including offscreen rows
    int64_t maxRowVol = 0;
    int firstCell = 0, cellCount = 0;
    int64_t maxSideVol = 0, lowRow = 0, highRow = 0;
};
struct FlowSnapshot {
    static constexpr int kMaxBars = 512, kMaxCells = 16384;
    uint64_t serial = 0;
    bool valid = false;
    int64_t timeframeMs = 0;
    double tick = 0, fromMs = 0, toMs = 0, low = 0, high = 0, maxRowVolume = 0;
    int barCount = 0, cellCount = 0;
    int skippedBars = 0, truncatedBars = 0;
    std::array<FlowColumn, kMaxBars> bars{};
    std::array<FlowCell, kMaxCells> cells{};
};
// Three fixed-capacity buffers: GUI back/pending, render current. No copying on sync.
class FlowMailbox {
public:
    explicit FlowMailbox(FootprintCoverage* coverage = nullptr);
    FlowSnapshot& back() { return *back_; }
    bool post();
    bool covers(const chart_raster::RasterCamera& cam) const { return coverage_ && coverage_->covers(cam); }
    bool take(std::unique_ptr<FlowSnapshot>& current);
private:
    FootprintCoverage* coverage_;
    std::mutex mutex_;
    std::unique_ptr<FlowSnapshot> back_, pending_;
    bool fresh_ = false;
};
struct FlowFeedPorts {
    std::function<MappingFrameContext()> frame;
    std::function<QString()> symbol;
    std::function<void()> update;
};
class FlowFeed final : public QObject {
public:
    static constexpr int kIntervalMs = 67;
    explicit FlowFeed(FlowMailbox& mailbox);
    void setPorts(FlowFeedPorts ports) { ports_ = std::move(ports); }
    void bind(ChartViewController&, const PublishedFrame&, LayerVisibility&, FootprintPresentation&);
    void bindSource(IGridDataSource&);
    void receive(SentinelStreamClient::FlowBatchPtr);
    void onFrameChanged();
    void publishRows();
    void setEnabled(bool);
    bool pending() const { return timer_.isActive(); }
    // Deterministic GUI-side binning; also usable by a later consumer of bar totals.
    static bool bin(FlowSnapshot&, const FlowSeriesBuffer::Series&, const MappingFrameContext&);
private:
    void selectSymbol();
    void withdraw();
    void post();
    bool visible(const MappingFrameContext&);
    void report(const FlowSnapshot&);
    FlowMailbox& mailbox_;
    FlowFeedPorts ports_;
    std::function<void()> coverageChanged_;
    FlowBufferAccess buffer_;
    QTimer timer_;
    QMetaObject::Connection sourceConnection_;
    QString symbol_;
    uint64_t serial_ = 0;
    bool enabled_ = false;
    FootprintStage lod_ = FootprintStage::Hidden;
};
} // namespace chart
