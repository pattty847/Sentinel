#include "chart/feeds/CvdFeed.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/shell/ChartTpoQml.hpp"
#include "datasources/IGridDataSource.hpp"
#include "render/GridViewState.hpp"
#include "servermodel/SessionManager.hpp"
namespace chart {
CvdMailbox::CvdMailbox() : back_(std::make_unique<CvdSnapshot>()), pending_(std::make_unique<CvdSnapshot>()) {}
void CvdMailbox::post() {
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
        presentation_.reset = visibility.cvdResetRule();
        presentation_.area = visibility.cvdStyle() == "area";
        onFrameChanged();
    };
    connect(&visibility, &LayerVisibility::layerVisibilityChanged, this, apply);
    connect(&tpo, &ChartTpoQml::tpoConfigChanged, this, &CvdFeed::onFrameChanged);
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
    if (!presentation_.stripPx) { timer_.stop(); return; }
    if (!timer_.isActive()) timer_.start(67);
}
void CvdFeed::receive(SentinelStreamClient::FlowBatchPtr batch) {
    if (!presentation_.stripPx) return;
    selectSymbol();
    if (!batch || batch->symbol != symbol_) return;
    buffer_.receive(*batch);
    onFrameChanged();
}
void CvdFeed::build(CvdSnapshot& out, const FlowSeriesBuffer::Series& source,
                    const MappingFrameContext& frame, CvdReset reset, int sessionType) {
    out.series.clear(); out.timeframeMs = frame.activeTimeframeMs > 0 ? std::max<int64_t>(1000, frame.activeTimeframeMs) : 0; out.reset = reset;
    const auto& cam = frame.raster;
    if (!cam.valid || out.timeframeMs <= 0) return;
    for (const auto& [time, record] : source.bars) {
        const auto& bar = record.bar;
        if (bar.baseScale <= 0 || bar.timeframeMs != out.timeframeMs) continue;
        const auto session = SessionManager::sessionContaining(time, static_cast<SessionManager::SessionType>(std::clamp(sessionType, 0, 6)));
        const auto boundary = CvdSeries::resetStart(time, reset, session.startMs);
        const bool visible = double(time) < cam.drawnEndMs && double(time) + out.timeframeMs > cam.drawnStartMs;
        out.series.push(time, double(bar.delta) / bar.baseScale, boundary, visible);
    }
}
void CvdFeed::publish() {
    selectSymbol();
    auto& out = mailbox_.back();
    out.series.clear(); out.reset = presentation_.reset;
    const auto frame = ports_.frame ? ports_.frame() : MappingFrameContext{};
    out.timeframeMs = frame.activeTimeframeMs > 0 ? std::max<int64_t>(1000, frame.activeTimeframeMs) : 0;
    if (presentation_.stripPx) {
        const auto* series = buffer_.series(symbol_, out.timeframeMs);
        if (series) build(out, *series, frame, presentation_.reset, ports_.sessionType ? ports_.sessionType() : 4);
    }
    mailbox_.post();
    if (ports_.update) ports_.update();
}
} // namespace chart
