#include "chart/feeds/CvdFeed.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/shell/ChartTpoQml.hpp"
#include "datasources/IGridDataSource.hpp"
#include "render/GridViewState.hpp"
#include "chart/core/CvdHistory.hpp"
#include "SentinelLogging.hpp"
#include <QDateTime>
#include <cstdio>
namespace chart {
namespace {
int64_t loadedStart(const FlowSeriesBuffer::Series& source, int64_t firstBar, int64_t boundary) {
    auto reached = firstBar;
    auto it = source.coverage.upper_bound(firstBar);
    while (it != source.coverage.begin() && reached > boundary) {
        const auto& range = (--it)->second;
        if (range.toMs < reached || (range.status != SentinelStreamClient::FlowStatus::Complete &&
                                   range.status != SentinelStreamClient::FlowStatus::Live)) break;
        reached = std::min(reached, range.fromMs);
    }
    return std::max(boundary, reached);
}
void buildVisible(CvdSnapshot& out, const FlowSeriesBuffer::Series& source, double from, double to) {
    if (!std::isfinite(from) || !std::isfinite(to) || from >= to ||
        from >= double(std::numeric_limits<int64_t>::max())) return;
    auto it = from <= double(std::numeric_limits<int64_t>::min()) ? source.bars.begin() :
        source.bars.lower_bound(int64_t(from));
    for (; it != source.bars.end() && double(it->first) < to; ++it) {
        const auto& bar = it->second.bar;
        if (bar.baseScale <= 0 || bar.timeframeMs != out.timeframeMs) continue;
        out.series.pushVisible(it->first, double(bar.delta) / bar.baseScale, from, to);
    }
}
void footerTotals(CvdSnapshot& out, const FlowSeriesBuffer::Series& source) {
    for (int i = 0; i < out.series.count; ++i) {
        auto& totals = out.totals[size_t(i)];
        totals = {};
        const auto it = source.bars.find(out.series.points[size_t(i)].timeMs);
        if (it == source.bars.end()) continue;
        const auto& bar = it->second.bar;
        if (bar.baseScale <= 0 || bar.timeframeMs != out.timeframeMs) continue;
        totals = {double(bar.delta) / bar.baseScale,
                  double(bar.sellVol) / bar.baseScale + double(bar.buyVol) / bar.baseScale, true};
    }
}
void baselineLabel(CvdSnapshot& out) {
    const char* label = out.reset == CvdReset::Visible ? "CVD since view start" :
        out.reset == CvdReset::UtcDay ? "CVD since UTC day" : "CVD since TPO session";
    std::snprintf(out.label.data(), out.label.size(), "%s", label);
    const auto loaded = out.series.incompleteStart();
    if (out.reset == CvdReset::Visible || loaded < 0 || !out.series.count) return;
    const auto at = QDateTime::fromMSecsSinceEpoch(loaded, Qt::UTC);
    const auto last = QDateTime::fromMSecsSinceEpoch(out.series.points[size_t(out.series.count - 1)].timeMs, Qt::UTC);
    const auto text = at.toString(at.date() == last.date() ? QStringLiteral("HH:mm") : QStringLiteral("yyyy-MM-dd HH:mm")).toLatin1();
    // The shared atlas has no parentheses; keep its default texture unchanged.
    std::snprintf(out.label.data(), out.label.size(), "CVD since %s loaded", text.constData());
}
} // namespace
CvdMailbox::CvdMailbox(CvdPresentation* presentation) : presentation_(presentation), back_(std::make_unique<CvdSnapshot>()), pending_(std::make_unique<CvdSnapshot>()) {}
void CvdMailbox::post() {
    if (presentation_) {
        presentation_->hasData = back_->series.count > 0;
        presentation_->dataTimeframeMs = back_->timeframeMs;
        presentation_->dataReset = back_->reset;
    }
    std::lock_guard lock(mutex_);
    back_.swap(pending_); fresh_ = true;
}
bool CvdMailbox::take(std::unique_ptr<CvdSnapshot>& current) {
    std::lock_guard lock(mutex_);
    if (!fresh_) return false;
    current.swap(pending_); fresh_ = false;
    return true;
}
CvdFeed::CvdFeed(CvdMailbox& mailbox, CvdPresentation& presentation) : mailbox_(mailbox), presentation_(presentation) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &CvdFeed::publish);
}
void CvdFeed::bind(ChartViewController& view, const PublishedFrame& published,
                   LayerVisibility& visibility, ChartTpoQml& tpo) {
    ports_ = {[&published] { return published.current(); }, [&view] { return view.session().symbol; },
              [&tpo] { return tpo.tpoSessionType(); }, [&visibility] { if (visibility.requestUpdate) visibility.requestUpdate(); }};
    const auto apply = [this, &visibility] {
        presentation_.stripPx = visibility.cvdStripPx();
        presentation_.flowFooter = visibility.flowFooter();
        presentation_.reset = visibility.cvdResetRule();
        presentation_.area = visibility.cvdStyle() == "area";
        const auto anchor = visibility.cvdFillAnchor();
        presentation_.fillAnchor = anchor == "zero" ? CvdFillAnchor::Zero :
            anchor == "edge" ? CvdFillAnchor::Edge :
            anchor == "none" ? CvdFillAnchor::None : CvdFillAnchor::Auto;
        onFrameChanged();
    };
    connect(&visibility, &LayerVisibility::layerVisibilityChanged, this, apply);
    connect(&tpo, &ChartTpoQml::tpoConfigChanged, this, [this, &visibility] {
        onFrameChanged();
        visibility.refreshCvdSessionHistory();
    });
    connect(view.viewPtr(), &GridViewState::viewportChanged, this, &CvdFeed::onFrameChanged);
    connect(view.viewPtr(), &GridViewState::panVisualOffsetChanged, this, &CvdFeed::onFrameChanged);
    apply();
}
void CvdFeed::bindSource(IGridDataSource& source) {
    disconnect(sourceConnection_);
    buffer_.bind(source);
    sourceConnection_ = connect(&source, &IGridDataSource::flowBarsReceived, this, &CvdFeed::receive);
    publish();
}
void CvdFeed::selectSymbol() {
    const auto symbol = ports_.symbol ? ports_.symbol() : QString{};
    if (symbol == symbol_) return;
    symbol_ = symbol; buffer_.clear();
    mailbox_.back().series.clear(); mailbox_.post();
}
void CvdFeed::onFrameChanged() {
    selectSymbol();
    if (!presentation_.stripPx && !presentation_.flowFooter) { timer_.stop(); return; }
    if (!timer_.isActive()) timer_.start(67);
}
void CvdFeed::receive(SentinelStreamClient::FlowBatchPtr batch) {
    selectSymbol();
    if (!batch || batch->symbol != symbol_) return;
    buffer_.receive(*batch);
    onFrameChanged();
}
int64_t CvdFeed::historyAnchor(int64_t start, int64_t timeframe) const {
    return cvdHistoryAnchor(start, timeframe, presentation_.stripPx || presentation_.flowFooter, presentation_.reset,
                            ports_.sessionType ? ports_.sessionType() : 4);
}
void CvdFeed::build(CvdSnapshot& out, const FlowSeriesBuffer::Series& source,
                    const MappingFrameContext& frame, CvdReset reset, int sessionType) {
    out.series.clear(); out.timeframeMs = frame.activeTimeframeMs > 0 ? std::max<int64_t>(1000, frame.activeTimeframeMs) : 0; out.reset = reset;
    const auto& cam = frame.raster;
    if (!cam.valid || out.timeframeMs <= 0) return;
    if (reset == CvdReset::Visible) {
        buildVisible(out, source, cam.drawnStartMs, cam.drawnEndMs);
        footerTotals(out, source);
        baselineLabel(out);
        return;
    }
    int64_t period = std::numeric_limits<int64_t>::min(), from = 0;
    for (const auto& [time, record] : source.bars) {
        const auto& bar = record.bar;
        if (bar.baseScale <= 0 || bar.timeframeMs != out.timeframeMs) continue;
        const auto boundary = cvdResetBoundary(time, reset, sessionType);
        if (boundary != period) { period = boundary; from = loadedStart(source, time, boundary); }
        const bool visible = double(time) < cam.drawnEndMs && double(time) + out.timeframeMs > cam.drawnStartMs;
        out.series.push(time, double(bar.delta) / bar.baseScale, boundary, visible, from);
    }
    footerTotals(out, source);
    baselineLabel(out);
}
void CvdFeed::publish() {
    selectSymbol();
    auto& out = mailbox_.back();
    out.series.clear(); out.reset = presentation_.reset;
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    out.timeframeMs = frame.activeTimeframeMs > 0 ? std::max<int64_t>(1000, frame.activeTimeframeMs) : 0;
    if (presentation_.stripPx || presentation_.flowFooter) {
        const auto* series = buffer_.series(symbol_, out.timeframeMs);
        if (series) build(out, *series, frame, presentation_.reset, ports_.sessionType ? ports_.sessionType() : 4);
    }
    if (out.series.overflow) {
        static sentinel::log_throttle::Site warning;
        uint32_t suppressed = 0;
        if (warning.admit(5000, sentinel::log_throttle::nowMs(), suppressed))
            sLog_Warning("CVD series full: symbol=" << symbol_ << " dropped=" << out.series.overflow
                         << sentinel::log_throttle::Suppressed{suppressed});
    }
    mailbox_.post();
    if (ports_.update) ports_.update();
}
} // namespace chart
