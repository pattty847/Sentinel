#include "HeatmapGpuLayer.hpp"
#include "HeatmapGpuSelfTest.hpp"
#include "SentinelLogging.hpp"
#include "heatmap/HeatmapSpanPlanner.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace heatmap::gpu {
namespace {
QString money(double price) { return QStringLiteral("$") + QString::number(price, 'g', 12); }
QString utc(int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("MM-dd HH:mm"));
}
QString utcSeconds(int64_t ms) {
    return ms ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss")) : QString();
}
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}
} // namespace

double medianRecentMid(const SparseColumns &data) {
    std::vector<double> mids;
    const size_t first = data.columns.size() > 30 ? data.columns.size() - 30 : 0;
    for (size_t c = first; c < data.columns.size(); ++c)
        for (const auto &n : data.columns[c].native) {
            int64_t bestBid = INT64_MIN, bestAsk = INT64_MAX;
            for (const auto &e : n.entries) {
                const int64_t row = n.baseRow + e.row();
                if (e.isAsk()) bestAsk = std::min(bestAsk, row);
                else bestBid = std::max(bestBid, row);
            }
            if (bestBid != INT64_MIN && bestAsk != INT64_MAX)
                mids.push_back(double(bestBid + bestAsk + 1) / 2 * n.grid.rowTickUnits / n.grid.priceScale);
        }
    if (mids.empty()) return 0;
    std::nth_element(mids.begin(), mids.begin() + mids.size() / 2, mids.end());
    return mids[mids.size() / 2];
}

HeatmapGpuLayer::HeatmapGpuLayer(QObject *parent) : QObject(parent) {
    refreshStyle();
    palette_ = makePalette(gradientsFor(settings_), tone_);
    labelRetry_ = new QTimer(this);
    labelRetry_->setSingleShot(true);
    connect(labelRetry_, &QTimer::timeout, this, &HeatmapGpuLayer::onLabelRetry);
}

HeatmapGpuLayer::~HeatmapGpuLayer() { setService(nullptr); }

void HeatmapGpuLayer::setService(HeatmapDataService *service) {
    if (service_ == service) return;
    destroyController();
    if (service_) service_->removeShutdownHook(serviceHook_);
    serviceHook_ = 0;
    service_ = service;
    if (service_) serviceHook_ = service_->addShutdownHook([this] { forgetService(); });
    if (active_) createController();
}

// The service dies before this layer (e.g. MainWindowGPU members before its
// widgets): drop the controller pointer without touching it; the service
// deletes it on its data thread. The node keeps its shared snapshot/capacity.
void HeatmapGpuLayer::forgetService() {
    if (controller_) QObject::disconnect(controller_, nullptr, this, nullptr);
    controller_ = nullptr;
    capacity_.reset();
    snapshot_.reset();
    live_.reset();
    resolution_.reset();
    resetLabels(false);
    service_ = nullptr;
    serviceHook_ = 0;
    sLog_App("Heatmap GPU layer: data service shut down first; controller forgotten");
}

void HeatmapGpuLayer::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (active) {
        prewarmPrecisionSelfTest(); // the fixture builds on a worker, not in prepare()
        createController();
    } else {
        destroyController();
    }
    noteLimits();
}

void HeatmapGpuLayer::createController() {
    if (controller_ || !service_ || !active_) return;
    controller_ = service_->createController(size_t(settings_.gpuCapBytes));
    capacity_ = controller_->capacity();
    connect(controller_, &HeatmapSourceController::snapshotChanged, this, [this] { onSnapshot(); },
            Qt::QueuedConnection);
    connect(controller_, &HeatmapSourceController::liveChanged, this, [this] { onLive(); }, Qt::QueuedConnection);
    connect(controller_, &HeatmapSourceController::buildFailed, this,
            [this](const QString &message) { emit buildFailed(message); }, Qt::QueuedConnection);
    if (auto *query = controller_->cellQuery()) {
        connect(query, &HeatmapCellQuery::labelsChanged, this, [this] { onLabels(); }, Qt::QueuedConnection);
        connect(query, &HeatmapCellQuery::queryFailed, this, [this](const QString &message) {
            ++labelCounters_.failures;
            sLog_Probe("heatmap.labels.failed", "message=" << message);
            if (!labelRetry_->isActive()) labelRetry_->start(retry_.delayMs());
        }, Qt::QueuedConnection);
    }
    postedTickUnits_ = -1;
    postLiveInterval();
    sLog_App("Heatmap GPU layer attached symbol=" << QString::fromStdString(symbol_) << " tfMs=" << tfMs_
             << " gpuCapBytes=" << settings_.gpuCapBytes);
    postView();
}

void HeatmapGpuLayer::destroyController() {
    if (!controller_) return;
    QObject::disconnect(controller_, nullptr, this, nullptr);
    if (service_) service_->destroyController(controller_);
    controller_ = nullptr;
    capacity_.reset();
    snapshot_.reset();
    live_.reset();
    resolution_.reset();
    tickUnits_ = autoUnits_ = 0;
    proposedTickUnits_.store(0, std::memory_order_relaxed);
    postedTickUnits_ = -1;
    tickKey_ = {};
    lastLiveVersion_ = 0;
    resetLabels(false);
    sLog_App("Heatmap GPU layer detached");
}

void HeatmapGpuLayer::setSymbol(const std::string &symbol) {
    if (symbol_ == symbol) return;
    symbol_ = symbol;
    autoPriceTick_.reset();
    invalidateTickProposal();
    lastLiveEndMs_ = 0;
    resetLabels(true); // the old symbol's labels never draw on the new picture
    autoUnits_ = 0; // Auto evaluates fresh on the new symbol's data
    restoreTick();
    noteLimits();
    viewChanged(); // the controller serial switches; the node holds the old picture
}

void HeatmapGpuLayer::setTimeframeMs(int64_t tfMs) {
    if (tfMs <= 0 || tfMs_ == tfMs) return;
    tfMs_ = tfMs;
    autoPriceTick_.reset();
    invalidateTickProposal();
    autoUnits_ = 0;
    restoreTick(); // Manual: this timeframe's remembered tick
    noteLimits();
    viewChanged(); // the old picture keeps drawing until the new timeframe's spans are ready
}

void HeatmapGpuLayer::setView(const ViewWindow &view, bool priceKnown) {
    if (hasView_ && view == view_ && priceKnown == priceKnown_) return;
    hasView_ = true;
    view_ = view;
    priceKnown_ = priceKnown;
    viewChanged();
}

void HeatmapGpuLayer::setSurface(double widthPx, double heightPx, double dpr) {
    widthPx_ = std::max(0.0, widthPx);
    heightPx_ = std::max(0.0, heightPx);
    dpr_ = dpr > 0 ? dpr : 1.0;
    noteLimits();
}

void HeatmapGpuLayer::setSettings(const HeatmapChartSettings &settings, bool explicitManualTick) {
    const auto previous = settings_;
    settings_ = settings;
    invalidateTickProposal(); // tick mode, Manual tick, minimum row height or hysteresis may change
    const bool wasManual = manualMode_;
    const int64_t wasUnits = manualUnits_;
    manualMode_ = settings.tickMode == TickMode::Manual;
    if (manualMode_) {
        if (explicitManualTick && isPresetUnits(settings.manualTick)) {
            manualUnits_ = settings.manualTick;
            tickMemory_.set(symbol_, tfMs_, manualUnits_);
        } else if (!wasManual) {
            const auto remembered = tickMemory_.get(symbol_, tfMs_);
            manualUnits_ = remembered ? *remembered : settings.manualTick;
        }
    }
    if (wasManual != manualMode_) autoUnits_ = 0; // back to Auto: the rule resumes from the current zoom
    if (previous.gpuCapBytes != settings.gpuCapBytes) postBudget();
    if (previous.liveMinIntervalMs != settings.liveMinIntervalMs) postLiveInterval();
    refreshStyle();
    if (previous.palettePreset != settings.palettePreset || previous.bidGradient != settings.bidGradient ||
        previous.askGradient != settings.askGradient)
        palette_ = makePalette(gradientsFor(settings_), tone_);
    if (wasManual != manualMode_ || wasUnits != manualUnits_) emit tickChanged();
    noteLimits();
}

void HeatmapGpuLayer::setTone(PaletteTone tone) {
    if (tone == tone_) return;
    tone_ = tone;
    palette_ = makePalette(gradientsFor(settings_), tone_);
}

void HeatmapGpuLayer::setTickMemory(const ManualTickMemory &memory) {
    tickMemory_ = memory;
    invalidateTickProposal();
    restoreTick();
    noteLimits();
}

void HeatmapGpuLayer::refreshStyle() {
    const recording::SizeScale scale; // the binner's output codes (Frame::outputScale)
    const auto window = codeWindow(settings_.sensitivityMin, settings_.sensitivityMax, scale.floor, scale.codesPerOctave);
    style_.codeFloor = window.floor;
    style_.codeRange = window.range;
    style_.opacity = float(std::clamp(settings_.opacity, 0.0, 1.0));
}

// Manual: the tick remembered for (symbol, timeframe) wins; with none, the locked
// tick stays (nothing is written: only an explicit choice is remembered).
void HeatmapGpuLayer::restoreTick() {
    if (!manualMode_) return;
    if (const auto units = tickMemory_.get(symbol_, tfMs_); units && *units != manualUnits_) {
        manualUnits_ = *units;
        emit tickChanged();
    }
}

double HeatmapGpuLayer::priceScale() const { return snapshot_ ? snapshot_->priceScale : 100.0; }

double HeatmapGpuLayer::maxTimeSpanMs() const {
    return active_ ? heatmap::maxTimeSpanMs(widthPx_ * dpr_, tfMs_) : 0.0;
}

bool HeatmapGpuLayer::priceScaleCurrent() const {
    // No snapshot yet: unknown (the 100 fallback is nobody's scale).
    return snapshot_ && snapshot_->symbol == symbol_ && snapshot_->priceScaleKnown;
}

double HeatmapGpuLayer::maxPriceSpan() const {
    if (!active_ || !manualMode_ || !isPresetUnits(manualUnits_) || !priceScaleCurrent()) return 0.0;
    return maxManualPriceSpan(heightPx_ * dpr_, fromUnits(manualUnits_, priceScale()));
}

double HeatmapGpuLayer::minTimeSpanMs() const { return active_ && tfMs_ > 0 ? double(kMinZoomColumns) * double(tfMs_) : 0.0; }

double HeatmapGpuLayer::minPriceSpan() const {
    if (!active_ || !priceScaleCurrent()) return 0.0;
    const int64_t units = manualMode_ && isPresetUnits(manualUnits_) ? manualUnits_ : (offered_.empty() ? 0 : offered_.front());
    return units > 0 ? kMinZoomRows * fromUnits(units, priceScale()) : 0.0;
}

void HeatmapGpuLayer::noteLimits() {
    const double time = maxTimeSpanMs(), price = maxPriceSpan(), minTime = minTimeSpanMs(), minPrice = minPriceSpan();
    if (time == lastMaxTime_ && price == lastMaxPrice_ && minTime == lastMinTime_ && minPrice == lastMinPrice_) return;
    lastMaxTime_ = time;
    lastMaxPrice_ = price;
    lastMinTime_ = minTime;
    lastMinPrice_ = minPrice;
    emit limitsChanged();
}

void HeatmapGpuLayer::viewChanged() {
    ++viewSerial_;
    postView();
}

void HeatmapGpuLayer::postView() {
    // Nothing is requested before the chart has a view (no placeholder window).
    if (!controller_ || !hasView_ || symbol_.empty() || tfMs_ <= 0 || !(view_.timeHiMs > view_.timeLoMs)) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, symbol = symbol_, tf = tfMs_, lo = view_.timeLoMs,
                                            hi = view_.timeHiMs] { c->setView(symbol, tf, lo, hi); },
                              Qt::QueuedConnection);
}

void HeatmapGpuLayer::postBudget() {
    if (!controller_) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, b = size_t(settings_.gpuCapBytes)] { c->setGpuBudget(b); },
                              Qt::QueuedConnection);
}

void HeatmapGpuLayer::postLiveInterval() {
    if (!controller_) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, ms = settings_.liveMinIntervalMs] { c->setLiveMinInterval(ms); },
                              Qt::QueuedConnection);
}

void HeatmapGpuLayer::onSnapshot() {
    if (!controller_) return;
    snapshot_ = controller_->latestSnapshot();
    resolution_ = controller_->latestResolution();
    if (!snapshot_) return;
    refreshPresets();
    noteLimits(); // the price scale may have changed
    emit snapshotChanged();
}

void HeatmapGpuLayer::onLive() {
    if (!controller_) return;
    resolution_ = controller_->latestResolution();
    live_ = controller_->latestLive();
    if (live_ && live_->version != lastLiveVersion_) {
        lastLiveVersion_ = live_->version;
        liveReceivedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    }
    if (live_ && live_->symbol == symbol_)
        for (const auto &source : live_->sources) lastLiveEndMs_ = std::max(lastLiveEndMs_, source.openEndMs);
    emit liveChanged();
}

int64_t HeatmapGpuLayer::liveOpenEndMs() const {
    if (!live_ || live_->tfMs != tfMs_ || live_->symbol != symbol_) return 0;
    int64_t end = 0;
    for (const auto &source : live_->sources) end = std::max(end, source.openEndMs);
    return end;
}

int64_t HeatmapGpuLayer::liveAnchorMs() const {
    int64_t anchor = std::max(lastLiveEndMs_, liveOpenEndMs());
    if (service_ && !symbol_.empty())
        if (const auto available = service_->availability(symbol_))
            for (const auto &source : available->sources)
                for (const auto &level : source.levels) anchor = std::max(anchor, level.committedThroughMs);
    return anchor;
}

int64_t HeatmapGpuLayer::oldestAvailableMs() const {
    int64_t oldest = 0;
    if (service_ && !symbol_.empty())
        if (const auto available = service_->availability(symbol_))
            for (const auto &source : available->sources)
                for (const auto &level : source.levels)
                    if (level.oldestMs > 0) oldest = oldest ? std::min(oldest, level.oldestMs) : level.oldestMs;
    return oldest;
}

void HeatmapGpuLayer::refreshPresets() {
    if (!snapshot_ || snapshot_->tfMs != tfMs_) return;
    int64_t finest = 0;
    for (const auto &column : snapshot_->resolution.columns)
        for (const auto &source : column.sources)
            if (source.commonUnits > 0) finest = finest ? std::min(finest, source.commonUnits) : source.commonUnits;
    if (!finest) return;
    auto offered = manualPresetUnits(snapshot_->resolution, finest * 1000);
    if (offered != offered_) {
        offered_ = std::move(offered);
        emit presetsChanged();
    }
}

// ------------------------------------------------------------------ frame
int64_t HeatmapGpuLayer::chooseTick(const ViewWindow &view) {
    if (!snapshot_ || snapshot_->tfMs != tfMs_ || snapshot_->symbol != symbol_) return tickUnits_; // nothing built yet
    if (manualMode_) return isPresetUnits(manualUnits_) ? manualUnits_ : tickUnits_;
    if (autoPriceTick_) return autoUnits_ = *autoPriceTick_;

    // Auto: the history summary merged with the live window's columns (S5L-b
    // latestResolution(); a live revision never replaces the SpanSet).
    const ResolutionSummary &summary = resolution_ && resolution_->tfMs == tfMs_ ? *resolution_ : snapshot_->resolution;
    const double heightPx = heightPx_ * dpr_;
    TickKey key{snapshot_->version, &summary, view, heightPx, settings_.minRowPx, settings_.hysteresis, autoUnits_};
    if (key == tickKey_) return autoUnits_ > 0 ? autoUnits_ : tickUnits_;
    const int64_t units = autoTickUnits(summary, autoUnits_, view.timeLoMs, view.timeHiMs, view.priceLo, view.priceHi,
                                        heightPx, {settings_.minRowPx, settings_.hysteresis});
    if (units > 0) autoUnits_ = units;
    key.current = autoUnits_;
    tickKey_ = key;
    return autoUnits_ > 0 ? autoUnits_ : tickUnits_;
}

void HeatmapGpuLayer::chooseTickForView(const ViewWindow &view) {
    if (!controller_) return;
    // GUI thread blocked: take the freshest snapshot, pick the tick; prepareFrame
    // hands both to the node in this frame (no thread hop on a tick change).
    if (auto latest = controller_->latestSnapshot(); latest && latest != snapshot_) snapshot_ = std::move(latest);
    live_ = controller_->latestLive(); // pointer reads; the node uploads only a new version
    resolution_ = controller_->latestResolution();
    const int64_t tick = priceKnown_ ? chooseTick(view) : 0; // nothing to draw before the price view exists
    // The drawn tick changes on the GUI thread (commitProposedTick), never here: the
    // chart then moves every layer that maps through the raster camera (axis models,
    // candles, paper and algo overlays) before the frame that draws the new rows, so no
    // frame mixes two mappings. This frame keeps the drawn tick.
    if (tick != proposedTickUnits_.load(std::memory_order_relaxed)) {
        proposedTickUnits_.store(tick, std::memory_order_relaxed);
        // Tagged with the tick policy it was chosen under (symbol, timeframe, settings,
        // Manual memory): a GUI-side change before the commit makes it stale.
        proposedPolicy_.store(tickPolicy_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        if (tick != tickUnits_) {
            sLog_Probe("heatmap.gpu.tick", "proposed tf=" << tfMs_ << " units=" << tick
                       << " mode=" << (manualMode_ ? "manual" : "auto"));
            QMetaObject::invokeMethod(this, [this] { commitProposedTick(); }, Qt::QueuedConnection);
        }
    }
    if (tickUnits_ != postedTickUnits_ || manualMode_ != postedManual_) {
        postedTickUnits_ = tickUnits_;
        postedManual_ = manualMode_;
        const auto mode = manualMode_ ? TickMode::Manual : TickMode::Auto;
        QMetaObject::invokeMethod(controller_, [c = controller_, mode, units = tickUnits_] { c->setTickRequest(mode, units); },
                                  Qt::QueuedConnection);
    }
}

void HeatmapGpuLayer::invalidateTickProposal() {
    // GUI thread: a pending proposal belongs to the old policy; the next frame chooses
    // afresh under the new one (and proposes even the same units again).
    tickPolicy_.fetch_add(1, std::memory_order_relaxed);
    proposedTickUnits_.store(tickUnits_, std::memory_order_relaxed);
}

void HeatmapGpuLayer::commitProposedTick() {
    // GUI thread (the render thread only reads tickUnits_ while the GUI thread is blocked).
    const int64_t tick = proposedTickUnits_.load(std::memory_order_relaxed);
    if (tick == tickUnits_) return; // superseded (the proposal went back to the drawn tick)
    if (proposedPolicy_.load(std::memory_order_relaxed) != tickPolicy_.load(std::memory_order_relaxed)) {
        sLog_Probe("heatmap.gpu.tick", "stale proposal dropped tf=" << tfMs_ << " units=" << tick);
        return; // chosen under another symbol, timeframe or tick setting: the next frame proposes again
    }
    tickUnits_ = tick;
    ++tickChanges_;
    sLog_Probe("heatmap.gpu.tick", "tf=" << tfMs_ << " units=" << tick << " mode=" << (manualMode_ ? "manual" : "auto"));
    emit tickChanged();
}

int64_t HeatmapGpuLayer::predictTickUnits(const ViewWindow &view) const {
    if (!snapshot_ || snapshot_->tfMs != tfMs_ || snapshot_->symbol != symbol_ || !priceKnown_) return tickUnits_;
    if (manualMode_) return isPresetUnits(manualUnits_) ? manualUnits_ : tickUnits_;
    if (autoPriceTick_) return *autoPriceTick_;

    const ResolutionSummary &summary = resolution_ && resolution_->tfMs == tfMs_ ? *resolution_ : snapshot_->resolution;
    const int64_t units = autoTickUnits(summary, autoUnits_, view.timeLoMs, view.timeHiMs, view.priceLo, view.priceHi,
                                        heightPx_ * dpr_, {settings_.minRowPx, settings_.hysteresis});
    return units > 0 ? units : autoUnits_ > 0 ? autoUnits_ : tickUnits_;
}

bool HeatmapGpuLayer::hasCurrentResolution() const {
    if (!snapshot_ || snapshot_->tfMs != tfMs_ || snapshot_->symbol != symbol_) return false;
    const auto &summary = resolution_ && resolution_->tfMs == tfMs_ ? *resolution_ : snapshot_->resolution;
    return summary.tfMs == tfMs_ && !summary.columns.empty();
}

bool HeatmapGpuLayer::buildsTick(int64_t units, const ViewWindow &candidate) const {
    if (!hasCurrentResolution()) return false;
    const auto &summary = resolution_ && resolution_->tfMs == tfMs_ ? *resolution_ : snapshot_->resolution;
    return buildsInView(summary, units, candidate.timeLoMs, candidate.timeHiMs, candidate.priceLo, candidate.priceHi);
}

bool HeatmapGpuLayer::setAutoPriceTick(std::optional<int64_t> units) {
    if (autoPriceTick_ == units) return false;
    autoPriceTick_ = units;
    tickKey_ = {};
    invalidateTickProposal(); // an older frame's proposal belongs to another raw fit
    return true;
}

bool HeatmapGpuLayer::prepareFrame(HeatmapTileNode::Frame &frame, const QRectF &rect, const ViewWindow &view,
                                   float coverageGamma, std::optional<ViewWindow> binView) {
    if (!controller_) return false;
    frame.spans = snapshot_;
    frame.capacity = capacity_;
    frame.tfMs = tfMs_;
    frame.tickUnits = tickUnits_;
    frame.view = view;
    frame.rect = rect;
    frame.coverageGamma = coverageGamma;
    frame.binView = binView;
    frame.uploadBudgetBytes = settings_.uploadBudgetBytes;
    frame.gpuCapBytes = settings_.gpuCapBytes;
    frame.style = style_;
    frame.palette = palette_;
    frame.crossfadeMs = settings_.crossfadeMs;
    frame.live = live_;
    frame.capture = capture_;
    renderedSerial_ = viewSerial_;
    postLabels(view);
    return true;
}

// ------------------------------------------------------------------ labels
void HeatmapGpuLayer::resetLabels(bool newEpoch) {
    labelsPosted_ = false;
    labelResultSeen_ = labelDropNoticed_ = false;
    postedLabels_ = {};
    notifiedLabels_.reset();
    retry_.succeeded();
    if (labelRetry_) labelRetry_->stop();
    if (newEpoch) {
        labelEpoch_ = labelSerial_ + 1;
        labelCounters_.epoch = labelEpoch_;
        if (liquidityRange_.valid) {
            liquidityRange_ = {};
            emit liquidityRangeChanged();
        }
    }
}

// Render thread, GUI thread blocked (prepareFrame). The key is the picture this
// frame targets (plan section 3): the chart's timeframe and the tick chosen for
// this frame, the current SpanSet and live snapshot. During a transition (hold or
// crossfade) that is the picture about to be drawn, so its labels are ready when
// the transition ends; labelsForFrame() draws them only once it is drawn.
void HeatmapGpuLayer::postLabels(const ViewWindow &view) {
    if (!controller_ || !controller_->cellQuery() || !snapshot_ || !priceKnown_) return;
    // The posted request's result was dropped without a signal (HeatmapCellQuery::
    // cancel: CPU shedding, a failed re-want): ask again after the backoff.
    const bool dropped = !controller_->cellQuery()->latestLabels();
    if (dropped) notifiedLabels_.reset(); // this frame draws none: the next result must redraw
    if (labelsPosted_ && labelResultSeen_ && !labelDropNoticed_ && dropped) {
        labelDropNoticed_ = true;
        QMetaObject::invokeMethod(this, [this] { labelRetry_->start(retry_.delayMs()); }, Qt::QueuedConnection);
    }
    const int64_t tf = tfMs_, tick = tickUnits_;
    if (snapshot_->tfMs != tf || snapshot_->symbol != symbol_ || tick <= 0 || tf < kMinuteMs) return;
    const double price = fromUnits(tick, snapshot_->priceScale);
    if (!(price > 0) || !std::isfinite(view.timeLoMs) || !std::isfinite(view.timeHiMs) || !std::isfinite(view.priceLo) ||
        !std::isfinite(view.priceHi) || !(view.timeHiMs > view.timeLoMs) || !(view.priceHi > view.priceLo))
        return;
    const double c0 = std::floor(view.timeLoMs / double(tf)), c1 = std::ceil(view.timeHiMs / double(tf));
    const double r0 = std::max(0.0, std::floor(view.priceLo / price)), r1 = std::ceil(view.priceHi / price);
    if (!(c1 > c0) || !(r1 > r0) || c0 < 0 || (c1 - c0) * (r1 - r0) > double(kMaxLabelCells)) return; // gate closed
    const bool live = live_ && live_->tfMs == tf && live_->symbol == symbol_;
    const auto &p = postedLabels_;
    if (labelsPosted_ && p.tfMs == tf && p.tickUnits == tick && p.priceScale == snapshot_->priceScale &&
        p.spanVersion == snapshot_->version && p.liveVersion == (live ? live_->version : 0) &&
        double(p.firstBucket) <= c0 && double(p.firstBucket + p.columns) >= c1 && double(p.firstBin) <= r0 &&
        double(p.firstBin + p.rows) >= r1)
        return; // the requested window still covers the view of the same picture
    // The view plus a margin on each side (pans inside it are layout only), at
    // most kMaxLabelCells cells.
    const double cols = c1 - c0, rows = r1 - r0;
    const double margin = std::clamp((std::sqrt(double(kMaxLabelCells) / (cols * rows)) - 1) / 2, 0.0, 0.5);
    LabelRequest request;
    request.serial = ++labelSerial_;
    request.spanVersion = snapshot_->version;
    request.liveVersion = live ? live_->version : 0;
    request.tfMs = tf;
    request.tickUnits = tick;
    request.priceScale = snapshot_->priceScale;
    request.firstBucket = int64_t(c0 - std::floor(cols * margin));
    request.columns = uint32_t(c1 + std::floor(cols * margin) - double(request.firstBucket));
    request.firstBin = int64_t(std::max(0.0, r0 - std::floor(rows * margin)));
    request.rows = uint32_t(r1 + std::floor(rows * margin) - double(request.firstBin));
    while (uint64_t(request.columns) * request.rows > kMaxLabelCells && request.rows > uint32_t(rows)) --request.rows;
    while (uint64_t(request.columns) * request.rows > kMaxLabelCells && request.columns > uint32_t(cols)) --request.columns;
    request.asset = symbol_.substr(0, symbol_.find('-'));
    request.formatText = true;
    postedLabels_ = request;
    labelsPosted_ = true;
    labelResultSeen_ = labelDropNoticed_ = false;
    ++labelCounters_.posted;
    labelCounters_.lastPostedSerial = request.serial;
    sLog_Probe("heatmap.labels.request", "serial=" << request.serial << " tf=" << tf << " tick=" << tick
               << " firstBucket=" << request.firstBucket << " columns=" << request.columns << " firstBin="
               << request.firstBin << " rows=" << request.rows << " span=" << request.spanVersion
               << " live=" << request.liveVersion);
    QMetaObject::invokeMethod(controller_, [c = controller_, request, spans = snapshot_,
                                            live = live ? live_ : std::shared_ptr<const LiveSnapshot>{}] {
        if (auto *query = c->cellQuery()) query->requestLabels(request, spans, live);
    }, Qt::QueuedConnection);
    // A result that never comes (a cancelled or shed query publishes nothing): the
    // retry reposts with backoff. Armed on the GUI thread, once per post.
    QMetaObject::invokeMethod(this, [this] { labelRetry_->start(retry_.delayMs()); }, Qt::QueuedConnection);
}

std::shared_ptr<const LabelCells> HeatmapGpuLayer::labelsForFrame() const {
    if (!controller_ || !settings_.showLabels || !controller_->cellQuery()) return {};
    // Re-read per frame: HeatmapCellQuery::cancel() drops its result without a
    // signal, so a held copy could outlive its picture.
    auto labels = controller_->cellQuery()->latestLabels();
    if (!labels) return {};
    // The node's drawn picture (its last prepare) and this frame's target must
    // both be the labels' timeframe and tick, with no crossfade: a transition
    // (hold or crossfade, which the node may start in this very frame) draws none.
    const auto &s = *tileStats_;
    if (labels->key.serial < labelEpoch_ || labels->key.tfMs != tfMs_ || s.drawnTfMs.load() != tfMs_ ||
        labels->key.tickUnits != tickUnits_ || s.drawnTickUnits.load() != tickUnits_ || s.crossfading.load())
        return {};
    return labels;
}

size_t HeatmapGpuLayer::matchLabelColumns(const LabelCells &labels, std::vector<uint8_t> &out) const {
    if (segmentScratch_.capacity() < 256) segmentScratch_.reserve(256);
    tileStats_->copySegments(segmentScratch_);
    return gpu::matchLabelColumns(labels, segmentScratch_, snapshot_.get(), live_.get(), out);
}

bool HeatmapGpuLayer::labelsPending() const {
    if (!controller_ || !settings_.showLabels || !controller_->cellQuery()) return false;
    const auto labels = controller_->cellQuery()->latestLabels();
    return labels && labels->key.serial >= labelEpoch_ && labels->key.tfMs == tfMs_ &&
           labels->key.tickUnits == tickUnits_ && !labelsForFrame();
}

uint64_t HeatmapGpuLayer::labelSignature() const {
    const auto labels = labelsForFrame();
    if (!labels) return 0;
    if (signatureColumns_.capacity() < kMaxLabelCells) signatureColumns_.reserve(kMaxLabelCells);
    const size_t matched = matchLabelColumns(*labels, signatureColumns_);
    return labelSignature(labels.get(), signatureColumns_, matched);
}

uint64_t HeatmapGpuLayer::labelSignature(const LabelCells *labels, const std::vector<uint8_t> &columns, size_t matched) {
    if (!labels) return 0;
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(labels)) * 1099511628211ull ^ labels->key.serial;
    for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i]) h = h * 1099511628211ull + i;
    return (h ^ matched) | 1; // nonzero for a drawable result
}

void HeatmapGpuLayer::onLabels() {
    if (!controller_ || !controller_->cellQuery()) return;
    const auto labels = controller_->cellQuery()->latestLabels();
    if (!labels || labels->key.serial < labelEpoch_) return;
    ++labelCounters_.results;
    if (labelsPosted_ && labels->key.serial == postedLabels_.serial) {
        labelRetry_->stop();
        retry_.succeeded();
        labelResultSeen_ = true;
        labelDropNoticed_ = false;
    }
    // The slider's ends: the window's largest cell, and its 5th percentile at the
    // low end (dust far below anything visible would waste most of the track).
    LiquidityRange range;
    liquidityScratch_.clear();
    for (const auto &cell : labels->cells)
        if (tiles::cellState(cell.word) == tiles::kCellValid && cell.value > 0 && std::isfinite(cell.value))
            liquidityScratch_.push_back(cell.value);
    if (liquidityScratch_.size() >= 2) {
        auto lo = liquidityScratch_.begin() + ptrdiff_t(liquidityScratch_.size() / 20);
        std::nth_element(liquidityScratch_.begin(), lo, liquidityScratch_.end());
        range.lo = *lo;
        range.hi = *std::max_element(liquidityScratch_.begin(), liquidityScratch_.end());
        range.valid = range.hi > range.lo;
    }
    // Ends move only by more than ~10% (live revisions nudge them every second).
    const auto moved = [](double a, double b) { return std::abs(std::log(a / b)) > 0.1; };
    if (range.valid && (!liquidityRange_.valid || moved(range.lo, liquidityRange_.lo) || moved(range.hi, liquidityRange_.hi))) {
        liquidityRange_ = range;
        emit liquidityRangeChanged();
    }
    // A republished picture with the same cells (an upload acknowledgement bumps
    // the SpanSet version) changes nothing on screen: no frame for it.
    const auto sameCells = [](const LabelCells &a, const LabelCells &b) {
        if (!(a.grid == b.grid) || a.cells.size() != b.cells.size() || a.key.liveVersion != b.key.liveVersion ||
            a.spanContent != b.spanContent || a.liveColumns != b.liveColumns)
            return false;
        for (size_t i = 0; i < a.cells.size(); ++i)
            if (a.cells[i].word != b.cells[i].word || a.cells[i].value != b.cells[i].value) return false;
        return true;
    };
    if (notifiedLabels_ && sameCells(*notifiedLabels_, *labels)) return;
    notifiedLabels_ = labels;
    emit labelsChanged();
}

// No result for the posted request: forget it so the next frame posts again, and
// back off (2 s doubling to 30 s) until one arrives.
void HeatmapGpuLayer::onLabelRetry() {
    if (!labelsPosted_ || !controller_ || !controller_->cellQuery()) return;
    const auto labels = controller_->cellQuery()->latestLabels();
    if (labels && labels->key.serial >= postedLabels_.serial) return;
    ++labelCounters_.retries;
    retry_.failed();
    sLog_Probe("heatmap.labels.retry", "serial=" << postedLabels_.serial << " nextMs=" << retry_.delayMs());
    labelsPosted_ = false;
    labelResultSeen_ = labelDropNoticed_ = false;
    emit labelsChanged(); // a frame, which posts again
}

bool HeatmapGpuLayer::settled() const {
    if (!controller_ || !priceKnown_ || !snapshot_ || snapshot_->tfMs != tfMs_ || snapshot_->symbol != symbol_ ||
        tickUnits_ <= 0)
        return false;
    if (renderedSerial_ != viewSerial_) return false; // the last frame showed an older view
    return tileStats_->complete.load() && tileStats_->drawnTickUnits.load() == tickUnits_ &&
           tileStats_->drawnTfMs.load() == tfMs_;
}

// The initial price: the median mid of the newest decoded minutes of the newest
// visible span (the chunk store holds them once the spans are built).
double HeatmapGpuLayer::recentMidPrice() const {
    if (!snapshot_ || !service_ || snapshot_->symbol != symbol_) return 0;
    const SpanSourceBuild *best = nullptr;
    int64_t bestTile = INT64_MIN;
    for (const auto &span : snapshot_->spans) {
        if (span.id.tfMs != tfMs_ || span.rank.tier != SpanTier::Visible) continue;
        for (const auto &source : span.sources)
            if (source.build && (span.id.tile > bestTile ||
                                 (span.id.tile == bestTile && best && source.build->commonUnits < best->commonUnits))) {
                best = source.build.get();
                bestTile = span.id.tile;
            }
    }
    if (!best) return 0;
    double price = 0;
    for (auto it = best->key.generations.rbegin(); it != best->key.generations.rend() && !(price > 0); ++it)
        if (const auto chunk = service_->store().cached({it->symbol, it->source, it->levelMs, it->startMs}))
            price = medianRecentMid(*chunk->columns);
    if (price > 0) return price;
    // No decoded minutes left: the centre of the source's coverage.
    for (const auto &column : best->resolution.columns)
        for (const auto &source : column.sources)
            if (!source.bands.empty())
                price = fromUnits((source.bands.front().lo + source.bands.back().end) / 2, snapshot_->priceScale);
    return price;
}

QString HeatmapGpuLayer::resolutionIndicator() const {
    if (!snapshot_ || snapshot_->tfMs != tfMs_ || tickUnits_ <= 0) return {};
    const auto ranges = veiledRanges(snapshot_->resolution, tickUnits_, view_.timeLoMs, view_.timeHiMs, view_.priceLo,
                                     view_.priceHi);
    if (ranges.empty()) return {};
    int64_t start = INT64_MAX, end = INT64_MIN, lo = INT64_MAX, hi = INT64_MIN;
    for (const auto &r : ranges) {
        start = std::min(start, r.startMs);
        end = std::max(end, r.endMs);
        lo = std::min(lo, r.price.lo);
        hi = std::max(hi, r.price.end);
    }
    const double scale = priceScale();
    return QStringLiteral("Resolution: %1 unavailable at %2-%3 in %4 to %5 UTC (no source builds it there). "
                          "Veiled, not coarsened.")
        .arg(money(fromUnits(tickUnits_, scale)), money(fromUnits(lo, scale)), money(fromUnits(hi, scale)), utc(start),
             utc(end));
}

QJsonObject HeatmapGpuLayer::state() const {
    const auto &s = *tileStats_;
    QJsonArray presets;
    for (const int64_t units : offered_) presets.append(qint64(units));
    std::vector<double> age, publish;
    for (const auto &[p, a] : s.liveSamples()) {
        publish.push_back(p);
        age.push_back(a);
    }
    QJsonObject out{{"settled", settled()},
                    {"tickUnits", qint64(tickUnits_)},
                    {"tick", tickPrice()},
                    {"tickMode", manualMode_ ? "manual" : "auto"},
                    {"manualTickUnits", qint64(manualUnits_)},
                    {"offeredPresets", presets},
                    {"drawnTimeframeMs", qint64(s.drawnTfMs.load())},
                    {"drawnTickUnits", qint64(s.drawnTickUnits.load())},
                    {"indicatorText", resolutionIndicator()},
                    {"residentBytes", qint64(s.residentBytes.load())},
                    {"gpuBytes", qint64(s.gpuBytes.load())},
                    {"liveVersion", qint64(s.liveVersion.load())},
                    {"liveAgeMs", s.liveDataAgeMs.load()},
                    {"liveAgeP50Ms", percentile(age, 0.5)},
                    {"liveAgeP95Ms", percentile(age, 0.95)},
                    {"livePublishP95Ms", percentile(publish, 0.95)},
                    {"liveSamples", qint64(age.size())},
                    {"prepareMs", s.prepareMs.load()},
                    {"lastBinMs", s.lastBinMs.load()},
                    {"gpuFrameMs", s.gpuFrameMs.load()},
                    {"slots", int(s.slotCount.load())},
                    {"readySlots", int(s.readySlots.load())},
                    {"loadingSlots", int(s.loadingSlots.load())},
                    {"holding", s.holding.load()},
                    {"errors", qint64(s.errors.load())},
                    {"missingDraws", qint64(s.missingDraws.load())},
                    {"tickChanges", qint64(tickChanges_)},
                    {"labels", QJsonObject{{"posted", qint64(labelCounters_.posted)},
                                           {"results", qint64(labelCounters_.results)},
                                           {"retries", qint64(labelCounters_.retries)},
                                           {"failures", qint64(labelCounters_.failures)},
                                           {"drawing", bool(labelsForFrame())},
                                           {"liquidityLo", liquidityRange_.valid ? liquidityRange_.lo : 0.0},
                                           {"liquidityHi", liquidityRange_.valid ? liquidityRange_.hi : 0.0}}},
                    {"snapshotVersion", qint64(snapshot_ ? snapshot_->version : 0)}};
    if (controllerStats_->valid) {
        const auto &c = controllerStats_->stats;
        const auto cache = service_ ? service_->stats().cache : SpanSourceCache::Stats{};
        out["controllerStats"] = QJsonObject{{"publications", qint64(c.publications)},
                                             {"admissions", qint64(c.admissions)},
                                             {"evictions", qint64(c.evictions)},
                                             {"suppressed", qint64(c.suppressed)},
                                             {"refused", qint64(c.refused)},
                                             {"committedBytes", qint64(c.committedBytes)},
                                             {"spanQueuedCancels", qint64(cache.queuedCancels)},
                                             {"spanRunningCancels", qint64(cache.runningCancels)},
                                             {"livePublications", qint64(c.livePublications)},
                                             {"liveComposeMs", c.liveComposeMs},
                                             {"liveIntervalMs", c.liveIntervalMs}};
    } else {
        out["controllerStats"] = QJsonValue(QJsonValue::Null);
    }
    refreshControllerStats();
    return out;
}

// Refreshes the controller's stats for the next read (they belong to its thread).
void HeatmapGpuLayer::scanWalls(const heatmap::WallQuery& query,
    std::function<void(heatmap::WallsSnapshot)> completion) {
    if (!controller_ || !snapshot_ || !priceKnown_ || snapshot_->tfMs != tfMs_ || snapshot_->symbol != symbol_) {
        heatmap::WallsSnapshot out; out.status = 503;
        completion(std::move(out));
        return;
    }
    const auto drawn = tileStats_->drawnTickUnits.load();
    WallScanRequest request{query, tfMs_, drawn > 0 ? drawn : tickUnits_, priceScale(),
                            view_.timeLoMs, view_.timeHiMs, view_.priceLo, view_.priceHi};
    QPointer<HeatmapGpuLayer> context(this);
    QMetaObject::invokeMethod(controller_, [c = controller_, request, spans = snapshot_, live = live_, context,
                                          completion = std::move(completion)]() mutable {
        if (context) c->cellQuery()->scanWalls(request, std::move(spans), std::move(live), context, std::move(completion));
    }, Qt::QueuedConnection);
}

void HeatmapGpuLayer::refreshControllerStats() const {
    if (!controller_) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, stats = controllerStats_] {
        const auto value = c->stats();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [stats, value] {
            stats->stats = value;
            stats->valid = true;
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void HeatmapGpuLayer::noteRaster(int rowPx, int colPx, double rowPxContinuous) {
    rowPxDrawn_.store(rowPx, std::memory_order_relaxed);
    colPxDrawn_.store(colPx, std::memory_order_relaxed);
    rowPxContinuous_.store(rowPxContinuous, std::memory_order_relaxed);
}

QVariantMap HeatmapGpuLayer::metrics() const {
    const auto &s = *tileStats_;
    const double tick = tickPrice();
    QVariantMap m{{"active", active_}, {"symbol", QString::fromStdString(symbol_)}, {"timeframeMs", qlonglong(tfMs_)},
                  {"mode", manualMode_ ? QStringLiteral("manual") : QStringLiteral("auto")},
                  {"tick", tick}, {"tickUnits", qlonglong(tickUnits_)},
                  {"manualTick", isPresetUnits(manualUnits_) ? fromUnits(manualUnits_, priceScale()) : 0.0},
                  {"hysteresis", settings_.hysteresis}, {"minRowPx", settings_.minRowPx},
                  {"crossfadeMs", settings_.crossfadeMs}, {"crossfading", s.crossfading.load()},
                  {"holding", s.holding.load()}, {"tickChanges", qulonglong(tickChanges_)},
                  {"rowPx", rowPxContinuous_.load(std::memory_order_relaxed)},
                  {"rowPxDrawn", rowPxDrawn_.load(std::memory_order_relaxed)},
                  {"colPxDrawn", colPxDrawn_.load(std::memory_order_relaxed)},
                  {"indicator", resolutionIndicator()}, {"settled", settled()},
                  {"lastBinMs", s.lastBinMs.load()}, {"prepareMs", s.prepareMs.load()},
                  {"gpuFrameMs", s.gpuFrameMs.load()}, {"gpuCapBytes", qulonglong(settings_.gpuCapBytes)},
                  {"gpuBytes", qulonglong(s.gpuBytes.load())}, {"residentBytes", qulonglong(s.residentBytes.load())},
                  {"sourceBytes", qulonglong(s.sourceBytes.load())}, {"binBytes", qulonglong(s.binBytes.load())},
                  {"residentSources", qulonglong(s.residentSources.load())},
                  {"sourcesUploaded", qulonglong(s.sourcesUploaded.load())},
                  {"binPasses", qulonglong(s.binPasses.load())}, {"fillPasses", qulonglong(s.fillPasses.load())},
                  {"evictions", qulonglong(s.evictions.load())}, {"missingReports", qulonglong(s.missingReports.load())},
                  {"readySlots", s.readySlots.load()}, {"fallbackSlots", s.fallbackSlots.load()},
                  {"partialSlots", s.partialSlots.load()}, {"loadingSlots", s.loadingSlots.load()},
                  {"kernel", s.preciseKernel.load() ? QStringLiteral("precise") : QStringLiteral("fast")},
                  {"refusedSpans", snapshot_ ? int(snapshot_->refused.size()) : 0},
                  {"refusedBytes", snapshot_ ? qulonglong(snapshot_->refusedBytes) : 0},
                  {"liveVersion", qulonglong(s.liveVersion.load())},
                  {"livePublished", live_ ? qulonglong(live_->version) : 0},
                  {"liveUploads", qulonglong(s.liveUploads.load())},
                  {"liveBinPasses", qulonglong(s.liveBinPasses.load())},
                  {"liveBufferCreations", qulonglong(s.liveBufferCreations.load())},
                  {"liveSets", s.liveSets.load()},
                  {"livePublishToDrawMs", s.livePublishToDrawMs.load()},
                  {"liveDataAgeMs", s.liveDataAgeMs.load()},
                  {"liveMinIntervalMs", settings_.liveMinIntervalMs},
                  {"liveL", utcSeconds(s.liveStartMs.load())}, {"liveEnd", utcSeconds(s.liveEndMs.load())},
                  {"liveFrom", utcSeconds(s.liveDrawFromMs.load())}, {"liveE", utcSeconds(s.liveEdgeCompleteMs.load())},
                  {"liveSpanMin", double(s.liveEndMs.load() - s.liveStartMs.load()) / 60'000.0}};
    {
        std::vector<double> publish, age;
        for (const auto &[p, a] : s.liveSamples()) {
            publish.push_back(p);
            age.push_back(a);
        }
        m["livePublishP50"] = percentile(publish, 0.5);
        m["livePublishP95"] = percentile(publish, 0.95);
        m["liveAgeP50"] = percentile(age, 0.5);
        m["liveAgeP95"] = percentile(age, 0.95);
    }
    // The finest source tick in view (rule 4: what Auto can reach here).
    if (snapshot_ && snapshot_->tfMs == tfMs_) {
        int64_t finest = 0;
        for (const auto &column : snapshot_->resolution.columns) {
            if (!(double(column.startMs) < view_.timeHiMs && double(column.startMs + tfMs_) > view_.timeLoMs)) continue;
            for (const auto &source : column.sources)
                if (source.state == BucketState::Present && source.commonUnits > 0 && !source.bands.empty())
                    finest = finest ? std::min(finest, source.commonUnits) : source.commonUnits;
        }
        m["commonTick"] = finest ? fromUnits(finest, snapshot_->priceScale) : 0.0;
    }
    if (service_) {
        const auto data = service_->stats();
        m.insert({{"connection", service_->connected() ? QStringLiteral("connected") : QStringLiteral("disconnected")},
                  {"chunkBytes", qulonglong(data.store.bytes)}, {"chunkEntries", qulonglong(data.store.entries)},
                  {"chunkLoads", qulonglong(data.store.loads)}, {"fetchedChunks", qulonglong(data.fetcher.requestedChunks)},
                  {"spanBuilds", qulonglong(data.cache.builds)}, {"spanCacheHits", qulonglong(data.cache.hits)},
                  {"spanLiveBytes", qlonglong(data.cache.liveBytes)}, {"spanCacheBytes", qulonglong(data.cache.bytes)},
                  {"spanClaimedBytes", qulonglong(data.cache.claimedBytes)},
                  {"spanReservedBytes", qulonglong(data.cache.reservedBytes)},
                  {"spanQueuedCancels", qulonglong(data.cache.queuedCancels)},
                  {"spanRunningCancels", qulonglong(data.cache.runningCancels)},
                  {"chunkWantedBytes", qulonglong(data.store.wantedBytes)},
                  {"cpuCommittedBytes", qulonglong(data.committedCpuBytes)}});
    }
    m["footprintBytes"] = qulonglong(processFootprintBytes());
    if (controllerStats_->valid) {
        const auto &c = controllerStats_->stats;
        m.insert({{"publications", qulonglong(c.publications)}, {"admissions", qulonglong(c.admissions)},
                  {"controllerEvictions", qulonglong(c.evictions)}, {"suppressed", qulonglong(c.suppressed)},
                  {"committedBytes", qulonglong(c.committedBytes)}, {"livePublications", qulonglong(c.livePublications)},
                  {"liveComposeMs", c.liveComposeMs}, {"liveIntervalMs", c.liveIntervalMs}});
    }
    refreshControllerStats();
    return m;
}
} // namespace heatmap::gpu
