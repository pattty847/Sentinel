#include "chart/feeds/FlowFeed.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/FootprintLod.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "datasources/IGridDataSource.hpp"
#include "render/GridViewState.hpp"
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <map>

namespace chart {
namespace {
bool grid(double native, double drawn) {
    if (!(native > 0) || !std::isfinite(native) || !(drawn > 0)) return false;
    const double ratio = std::max(native, drawn) / std::min(native, drawn);
    return std::isfinite(ratio) && ratio < 1e9 && std::abs(ratio - std::round(ratio)) < 1e-6;
}
bool addBar(FlowSnapshot& out, const sentinel::flow::FlowBar& bar) {
    if (bar.baseScale <= 0 || !grid(bar.nativeTick, out.tick)) return false;
    if (out.barCount == FlowSnapshot::kMaxBars) return false;
    auto& column = out.bars[size_t(out.barCount++)];
    column = {bar.startMs, bar.baseScale, bar.sellVol, bar.buyVol, bar.delta, 0, out.cellCount, 0};
    const int64_t span = std::max<int64_t>(1, std::llround(bar.nativeTick / out.tick));
    std::map<int64_t, FlowCell> bins;
    for (const auto& row : bar.rows) {
        const double price = (double(bar.lowRow) + row.offset) * bar.nativeTick;
        const double index = std::floor(price / out.tick + 1e-7);
        if (!std::isfinite(index) || std::abs(index) > 9e15 || row.bidVol < 0 || row.askVol < 0) return false;
        auto& cell = bins[int64_t(index)];
        cell.row = int64_t(index); cell.span = span;
        cell.bidVol += row.bidVol; cell.askVol += row.askVol;
    }
    for (const auto& [index, cell] : bins) {
        column.maxRowVol = std::max(column.maxRowVol, cell.bidVol + cell.askVol);
        if ((double(index) + span) * out.tick <= out.low || double(index) * out.tick >= out.high) continue;
        if (out.cellCount == FlowSnapshot::kMaxCells) return false;
        out.cells[size_t(out.cellCount++)] = cell;
        ++column.cellCount;
        out.maxRowVolume = std::max(out.maxRowVolume, double(cell.bidVol + cell.askVol) / bar.baseScale);
    }
    return true;
}
} // namespace
FlowMailbox::FlowMailbox() : back_(std::make_unique<FlowSnapshot>()), pending_(std::make_unique<FlowSnapshot>()) {}
void FlowMailbox::post() {
    std::lock_guard lock(mutex_);
    back_.swap(pending_);
    fresh_ = true;
}
bool FlowMailbox::take(std::unique_ptr<FlowSnapshot>& current) {
    std::lock_guard lock(mutex_);
    if (!fresh_) return false;
    current.swap(pending_);
    fresh_ = false;
    return true;
}
FlowFeed::FlowFeed(FlowMailbox& mailbox) : mailbox_(mailbox) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &FlowFeed::publishRows);
}
void FlowFeed::bind(ChartViewController& view, const PublishedFrame& published,
                    LayerVisibility& visibility, FootprintPresentation& presentation) {
    ports_.frame = [&published] { return published.current(); };
    ports_.symbol = [&view] { return view.session().symbol; };
    ports_.update = [&visibility] { if (visibility.requestUpdate) visibility.requestUpdate(); };
    connect(&visibility, &LayerVisibility::layerVisibilityChanged, this, [this, &visibility, &presentation] {
        presentation.enabled = visibility.footprintEnabled();
        presentation.numbers = visibility.footprintNumbers();
        presentation.hollowCandles = visibility.footprintHollowCandles();
        presentation.windowIntensity = visibility.footprintIntensity() == "window";
        setEnabled(presentation.enabled);
    });
    connect(view.viewPtr(), &GridViewState::viewportChanged, this, &FlowFeed::onFrameChanged);
    connect(view.viewPtr(), &GridViewState::panVisualOffsetChanged, this, &FlowFeed::onFrameChanged);
}
void FlowFeed::bindSource(IGridDataSource& source) {
    disconnect(sourceConnection_);
    buffer_.clear();
    sourceConnection_ = connect(&source, &IGridDataSource::flowBarsReceived, this, &FlowFeed::receive);
    publishRows();
}
void FlowFeed::selectSymbol() {
    const auto symbol = ports_.symbol ? ports_.symbol() : QString{};
    if (symbol == symbol_) return;
    symbol_ = symbol;
    buffer_.clear();
    auto& out = mailbox_.back();
    out.valid = false; out.barCount = 0; out.cellCount = 0; out.serial = ++serial_;
    mailbox_.post();
}
void FlowFeed::receive(SentinelStreamClient::FlowBatchPtr batch) {
    selectSymbol();
    if (!batch || batch->symbol != symbol_) return;
    buffer_.apply(*batch, QDateTime::currentMSecsSinceEpoch());
    onFrameChanged();
}
void FlowFeed::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) onFrameChanged(); else { timer_.stop(); publishRows(); }
}
void FlowFeed::onFrameChanged() {
    selectSymbol();
    if (enabled_ && !timer_.isActive()) timer_.start(kIntervalMs);
}
bool FlowFeed::bin(FlowSnapshot& out, const FlowSeriesBuffer::Series& series, const MappingFrameContext& frame) {
    const auto& cam = frame.raster;
    out.valid = false; out.barCount = 0; out.cellCount = 0; out.maxRowVolume = 0;
    out.tick = cam.tick; out.timeframeMs = frame.activeTimeframeMs;
    out.fromMs = cam.drawnStartMs; out.toMs = cam.drawnEndMs;
    out.low = cam.drawnMinPrice; out.high = cam.drawnMaxPrice;
    if (!cam.valid || !std::isfinite(cam.tick) || cam.tick <= 0 || out.timeframeMs < 1000) return false;
    for (const auto& [start, record] : series.bars) {
        const auto& bar = record.bar;
        if (double(start) >= out.toMs) break;
        if (double(start) + bar.timeframeMs <= out.fromMs) continue;
        if (bar.timeframeMs != out.timeframeMs || !addBar(out, bar)) return false;
    }
    out.valid = true;
    return true;
}
void FlowFeed::publishRows() {
    selectSymbol();
    auto& out = mailbox_.back();
    out.valid = false; out.barCount = 0; out.cellCount = 0;
    out.serial = ++serial_;
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    const auto* series = buffer_.series(frame.activeTimeframeMs);
    if (enabled_ && series) bin(out, *series, frame);
    mailbox_.post();
    if (ports_.update) ports_.update();
}
} // namespace chart
