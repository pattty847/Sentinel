#include "chart/feeds/FlowFeed.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/FootprintLod.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "datasources/IGridDataSource.hpp"
#include "render/GridViewState.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include "SentinelLogging.hpp"

namespace chart {
namespace {
bool grid(double native, double drawn) {
    if (!(native > 0) || !std::isfinite(native) || !(drawn > 0)) return false;
    const double ratio = std::max(native, drawn) / std::min(native, drawn);
    return std::isfinite(ratio) && ratio < 1e9 && std::abs(ratio - std::round(ratio)) < 1e-6;
}
using FlatRows = std::array<FlowCell, FlowSnapshot::kMaxCells>;
enum class Added { Yes, Invalid, Oversized, Full };
int prepareRows(FlatRows& rows, const sentinel::flow::FlowBar& bar, double tick) {
    if (bar.baseScale <= 0 || !grid(bar.nativeTick, tick)) return -1;
    if (bar.rows.size() > rows.size()) return -2;
    const int64_t span = std::max<int64_t>(1, std::llround(bar.nativeTick / tick));
    int count = 0;
    for (const auto& row : bar.rows) {
        const double index = std::floor((double(bar.lowRow) + row.offset) * bar.nativeTick / tick + 1e-7);
        if (!std::isfinite(index) || std::abs(index) > 9e15 || row.bidVol < 0 || row.askVol < 0 ||
            row.bidVol > INT64_MAX - row.askVol) return -1;
        rows[size_t(count++)] = {int64_t(index), span, row.bidVol, row.askVol};
    }
    std::sort(rows.begin(), rows.begin() + count, [](const auto& a, const auto& b) { return a.row < b.row; });
    return count;
}
// Coarse native bins stay whole: a band spans a multiple of the native span.
void bandRows(FlatRows& rows, int count, int64_t factor) {
    if (count == 0 || factor <= 1) return;
    const int64_t span = rows[0].span, band = span * ((factor + span - 1) / span);
    if (band <= span) return;
    for (int i = 0; i < count; ++i) rows[size_t(i)] = {footprintBandStart(rows[size_t(i)].row, band), band,
                                                        rows[size_t(i)].bidVol, rows[size_t(i)].askVol};
}
int mergeRows(FlatRows& rows, int count) {
    int used = 0;
    for (int i = 0; i < count; ++i) {
        const auto row = rows[size_t(i)];
        if (!used || rows[size_t(used - 1)].row != row.row) rows[size_t(used++)] = row;
        else {
            auto& held = rows[size_t(used - 1)];
            if (held.bidVol + held.askVol > INT64_MAX - (row.bidVol + row.askVol)) return -1;
            held.bidVol += row.bidVol; held.askVol += row.askVol;
        }
    }
    return used;
}
bool inPrice(const FlowCell& row, double tick, double low, double high) {
    return (double(row.row) + row.span) * tick > low && double(row.row) * tick < high;
}
Added addBar(FlowSnapshot& out, const sentinel::flow::FlowBar& bar, FlatRows& rows,
             const chart_raster::RasterCamera& cam, int64_t band) {
    int count = prepareRows(rows, bar, out.tick);
    if (count == -2) return Added::Oversized;
    if (count >= 0) bandRows(rows, count, band);
    if (count < 0 || (count = mergeRows(rows, count)) < 0) return Added::Invalid;
    FlowColumn column{bar.startMs, bar.baseScale, bar.sellVol, bar.buyVol, bar.delta, 0, out.cellCount, 0};
    if (count) { column.lowRow = rows[0].row; column.highRow = rows[size_t(count - 1)].row + rows[size_t(count - 1)].span; }
    for (int i = 0; i < count; ++i) {
        const auto& cell = rows[size_t(i)];
        column.maxSideVol = std::max({column.maxSideVol, cell.bidVol, cell.askVol});
        column.maxRowVol = std::max(column.maxRowVol, cell.bidVol + cell.askVol);
        if (inPrice(cell, out.tick, out.low, out.high)) ++column.cellCount;
    }
    if (out.barCount == FlowSnapshot::kMaxBars || out.cellCount + column.cellCount > FlowSnapshot::kMaxCells)
        return Added::Full;
    out.bars[size_t(out.barCount++)] = column;
    const bool visibleBar = double(bar.startMs) < cam.drawnEndMs && double(bar.startMs) + bar.timeframeMs > cam.drawnStartMs;
    for (int i = 0; i < count; ++i) {
        const auto& cell = rows[size_t(i)];
        if (!inPrice(cell, out.tick, out.low, out.high)) continue;
        out.cells[size_t(out.cellCount++)] = cell;
        if (visibleBar && inPrice(cell, out.tick, cam.drawnMinPrice, cam.drawnMaxPrice))
            out.maxRowVolume = std::max(out.maxRowVolume, double(cell.bidVol + cell.askVol) / bar.baseScale);
    }
    return Added::Yes;
}
bool validWindow(const FlowSnapshot& out, const chart_raster::RasterCamera& cam, double width, double height) {
    return cam.valid && std::isfinite(cam.tick) && cam.tick > 0 && out.timeframeMs >= 1000 &&
        std::isfinite(cam.drawnStartMs) && std::isfinite(cam.drawnMinPrice) &&
        std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0;
}
struct Budget { int64_t band = 1; double padMs = 0, padPrice = 0; };
// Bands coarsen until every visible bar's worst case fits kMaxCells; only overscan
// shrinks to fit. Visible bar and row counts depend on the view size, not the pan.
Budget budget(const FlowSnapshot& out, const chart_raster::RasterCamera& cam, double width, double height,
              FootprintStage stage, int64_t previous) {
    const double tf = double(out.timeframeMs);
    const double bars = std::min(std::ceil(width / tf) + 1, 1e12), rows = std::min(std::ceil(height / cam.tick) + 1, 1e12);
    Budget plan;
    plan.band = std::max(footprintBandRows(cam.rowPxF / cam.dpr, stage, previous),
                         footprintBudgetBandRows(int64_t(bars), int64_t(rows), FlowSnapshot::kMaxCells));
    const double perBar = double(std::max<int64_t>(1, FlowSnapshot::kMaxCells / int64_t(bars) - 1));
    plan.padPrice = std::clamp((perBar * double(plan.band) - rows) * 0.5, 0.0, height / cam.tick) * cam.tick;
    plan.padMs = std::clamp(((FlowSnapshot::kMaxBars - 2) * tf - width) * 0.5, 0.0, width);
    return plan;
}
Added visit(FlowSnapshot& out, const sentinel::flow::FlowBar& bar, FlatRows& rows,
            const chart_raster::RasterCamera& cam, int64_t band) {
    const auto result = bar.timeframeMs == out.timeframeMs ? addBar(out, bar, rows, cam, band) : Added::Invalid;
    if (result == Added::Invalid) ++out.skippedBars;
    if (result == Added::Oversized || result == Added::Full) ++out.truncatedBars;
    return result;
}
int64_t barKey(double ms) { return int64_t(std::clamp(ms, -9e15, 9e15)); }
// Visible bars newest first, so a full snapshot trims the far past and never the live
// edge; then overscan outward from the nearer view edge. No candidate allocation.
void collect(FlowSnapshot& out, const FlowSeriesBuffer::Series& series, FlatRows& rows,
             const chart_raster::RasterCamera& cam, int64_t band) {
    const double tf = double(out.timeframeMs);
    const auto lo = series.bars.upper_bound(barKey(std::floor(cam.drawnStartMs - tf)));
    const auto hi = series.bars.lower_bound(barKey(std::ceil(cam.drawnEndMs)));
    for (auto it = hi; it != lo;)
        if (visit(out, (--it)->second.bar, rows, cam, band) == Added::Full) return;
    auto left = lo, right = hi;
    for (;;) {
        const bool leftOk = left != series.bars.begin() && double(std::prev(left)->first) + tf > out.fromMs;
        const bool rightOk = right != series.bars.end() && double(right->first) < out.toMs;
        if (!leftOk && !rightOk) return;
        const bool takeLeft = leftOk && (!rightOk ||
            cam.drawnStartMs - tf - double(std::prev(left)->first) <= double(right->first) - cam.drawnEndMs);
        const auto& bar = takeLeft ? (--left)->second.bar : (right++)->second.bar;
        if (visit(out, bar, rows, cam, band) == Added::Full) return;
    }
}
} // namespace
FlowMailbox::FlowMailbox(FootprintCoverage* coverage) : coverage_(coverage), back_(std::make_unique<FlowSnapshot>()), pending_(std::make_unique<FlowSnapshot>()) {}
bool FlowMailbox::post() {
    bool changed = false;
    if (coverage_) {
        changed = coverage_->tick != back_->tick || coverage_->timeframeMs != back_->timeframeMs;
        const int oldCount = coverage_->barCount;
        coverage_->barCount = 0;
        coverage_->tick = back_->tick; coverage_->timeframeMs = back_->timeframeMs;
        if (back_->valid) for (int i = 0; i < back_->barCount; ++i) {
            const auto& bar = back_->bars[size_t(i)];
            if (bar.cellCount) {
                const FootprintCoverage::Bar next{double(bar.startMs), double(bar.startMs) + back_->timeframeMs,
                    bar.lowRow * back_->tick, bar.highRow * back_->tick};
                auto& previous = coverage_->bars[size_t(coverage_->barCount++)];
                changed = changed || previous != next;
                previous = next;
            }
        }
        changed = changed || oldCount != coverage_->barCount;
        if (changed) ++coverage_->revision;
    }
    std::lock_guard lock(mutex_);
    back_.swap(pending_);
    fresh_ = true;
    return changed;
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
    coverageChanged_ = [&visibility] { emit visibility.layerVisibilityChanged(); };
    const auto apply = [this, &visibility, &presentation] {
        presentation.enabled = visibility.footprintEnabled();
        presentation.cellsSource = visibility.footprintSource() == "cells";
        presentation.numbers = visibility.footprintNumbers();
        const bool numbersChanged = numbers_ != presentation.numbers;
        numbers_ = presentation.numbers;
        presentation.spineCandles = visibility.footprintCandle() == "spine";
        presentation.dustNotional = visibility.footprintDustNotional();
        presentation.windowIntensity = visibility.footprintIntensity() == "window";
        setEnabled(presentation.enabled && presentation.cellsSource);
        if (numbersChanged) onFrameChanged(); // band height follows the numbers setting
    };
    connect(&visibility, &LayerVisibility::layerVisibilityChanged, this, apply);
    apply();
    connect(view.viewPtr(), &GridViewState::viewportChanged, this, &FlowFeed::onFrameChanged);
    connect(view.viewPtr(), &GridViewState::panVisualOffsetChanged, this, &FlowFeed::onFrameChanged);
}
void FlowFeed::bindSource(IGridDataSource& source) {
    disconnect(sourceConnection_);
    buffer_.bind(source);
    sourceConnection_ = connect(&source, &IGridDataSource::flowBarsReceived, this, &FlowFeed::receive);
    if (enabled_) publishRows();
}
void FlowFeed::selectSymbol() {
    const auto symbol = ports_.symbol ? ports_.symbol() : QString{};
    if (symbol == symbol_) return;
    symbol_ = symbol;
    buffer_.clear();
    withdraw();
}
void FlowFeed::post() {
    // Partial history can change one candle even when the viewport as a whole
    // remains uncovered. Notify only when per-bar coverage actually changes.
    if (mailbox_.post() && coverageChanged_) coverageChanged_();
}
void FlowFeed::withdraw() {
    auto& out = mailbox_.back();
    out.valid = false; out.barCount = 0; out.cellCount = 0; out.serial = ++serial_;
    post();
}
void FlowFeed::receive(SentinelStreamClient::FlowBatchPtr batch) {
    if (!enabled_ || lod_ == FootprintStage::Hidden) return;
    selectSymbol();
    if (!batch || batch->symbol != symbol_) return;
    buffer_.receive(*batch);
    onFrameChanged();
}
void FlowFeed::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) onFrameChanged();
    else { timer_.stop(); buffer_.clear(); withdraw(); }
}
void FlowFeed::onFrameChanged() {
    if (!enabled_) return;
    selectSymbol();
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    if (!visible(frame)) return;
    if (!timer_.isActive()) timer_.start(kIntervalMs);
}
bool FlowFeed::visible(const MappingFrameContext& frame) {
    const auto& cam = frame.raster;
    const auto next = cam.valid && cam.dpr > 0 && frame.activeTimeframeMs >= 1000
        ? footprintLod(cam.colPxF / cam.dpr, cam.rowPxF / cam.dpr, lod_) : FootprintStage::Hidden;
    if (next == FootprintStage::Hidden) {
        timer_.stop();
        if (lod_ != next) withdraw();
    }
    lod_ = next;
    return lod_ != FootprintStage::Hidden;
}
bool FlowFeed::bin(FlowSnapshot& out, const FlowSeriesBuffer::Series& series, const MappingFrameContext& frame) {
    const auto& cam = frame.raster;
    return bin(out, series, frame, cam.dpr > 0 ? footprintLod(cam.colPxF / cam.dpr, cam.rowPxF / cam.dpr) : FootprintStage::Hidden);
}
bool FlowFeed::bin(FlowSnapshot& out, const FlowSeriesBuffer::Series& series, const MappingFrameContext& frame,
                   FootprintStage stage, int64_t previousBand) {
    const auto& cam = frame.raster;
    out.valid = false; out.barCount = 0; out.cellCount = 0; out.maxRowVolume = 0;
    out.skippedBars = 0; out.truncatedBars = 0; out.bandRows = 1;
    out.tick = cam.tick; out.timeframeMs = frame.activeTimeframeMs;
    const double width = cam.drawnEndMs - cam.drawnStartMs, height = cam.drawnMaxPrice - cam.drawnMinPrice;
    if (!validWindow(out, cam, width, height)) return false;
    const auto plan = budget(out, cam, width, height, stage, previousBand);
    out.bandRows = plan.band;
    out.fromMs = cam.drawnStartMs - plan.padMs; out.toMs = cam.drawnEndMs + plan.padMs;
    out.low = cam.drawnMinPrice - plan.padPrice; out.high = cam.drawnMaxPrice + plan.padPrice;
    // Reused on the GUI thread; fixed storage also bounds malformed/oversized bars.
    thread_local FlatRows rows;
    collect(out, series, rows, cam, plan.band);
    std::sort(out.bars.begin(), out.bars.begin() + out.barCount,
              [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
    out.valid = true;
    return true;
}
void FlowFeed::report(const FlowSnapshot& out) {
    sLog_Probe("footprint.feed", "symbol=" << symbol_ << " bars=" << out.barCount << " cells=" << out.cellCount
               << " skipped=" << out.skippedBars << " truncated=" << out.truncatedBars);
    if (!out.skippedBars && !out.truncatedBars) return;
    static sentinel::log_throttle::Site warning;
    uint32_t suppressed = 0;
    if (warning.admit(5000, sentinel::log_throttle::nowMs(), suppressed))
        sLog_Warning("Footprint snapshot incomplete: symbol=" << symbol_ << " skipped=" << out.skippedBars
                     << " truncated=" << out.truncatedBars << sentinel::log_throttle::Suppressed{suppressed});
}
void FlowFeed::publishRows() {
    if (!enabled_) return;
    selectSymbol();
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    if (!visible(frame)) return;
    auto& out = mailbox_.back();
    out.valid = false; out.barCount = 0; out.cellCount = 0;
    out.skippedBars = 0; out.truncatedBars = 0; out.serial = ++serial_;
    const auto* series = buffer_.series(symbol_, frame.activeTimeframeMs);
    // Numbers off caps the stage at Cells, as the layer does, so bands stay at the Cells height.
    const auto stage = !numbers_ && lod_ == FootprintStage::Numbers ? FootprintStage::Cells : lod_;
    if (series && bin(out, *series, frame, stage, band_)) band_ = out.bandRows;
    report(out);
    post();
    if (ports_.update) ports_.update();
}
} // namespace chart
