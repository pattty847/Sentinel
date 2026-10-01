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
}

HeatmapGpuLayer::~HeatmapGpuLayer() { destroyController(); }

void HeatmapGpuLayer::setService(HeatmapDataService *service) {
    if (service_ == service) return;
    destroyController();
    service_ = service;
    if (active_) createController();
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
    postedTickUnits_ = -1;
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
    postedTickUnits_ = -1;
    tickKey_ = {};
    lastLiveVersion_ = 0;
    sLog_App("Heatmap GPU layer detached");
}

void HeatmapGpuLayer::setSymbol(const std::string &symbol) {
    if (symbol_ == symbol) return;
    symbol_ = symbol;
    autoUnits_ = 0; // Auto evaluates fresh on the new symbol's data
    restoreTick();
    noteLimits();
    viewChanged(); // the controller serial switches; the node holds the old picture
}

void HeatmapGpuLayer::setTimeframeMs(int64_t tfMs) {
    if (tfMs <= 0 || tfMs_ == tfMs) return;
    tfMs_ = tfMs;
    autoUnits_ = 0;
    restoreTick(); // Manual: this timeframe's remembered tick
    noteLimits();
    viewChanged(); // the old picture keeps drawing until the new timeframe's spans are ready
}

void HeatmapGpuLayer::setView(const ViewWindow &view, bool priceKnown) {
    if (view == view_ && priceKnown == priceKnown_) return;
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

double HeatmapGpuLayer::maxPriceSpan() const {
    if (!active_ || !manualMode_ || !isPresetUnits(manualUnits_)) return 0.0;
    return maxManualPriceSpan(heightPx_ * dpr_, fromUnits(manualUnits_, priceScale()));
}

void HeatmapGpuLayer::noteLimits() {
    const double time = maxTimeSpanMs(), price = maxPriceSpan();
    if (time == lastMaxTime_ && price == lastMaxPrice_) return;
    lastMaxTime_ = time;
    lastMaxPrice_ = price;
    emit limitsChanged();
}

void HeatmapGpuLayer::viewChanged() {
    ++viewSerial_;
    postView();
}

void HeatmapGpuLayer::postView() {
    if (!controller_ || symbol_.empty() || tfMs_ <= 0 || !(view_.timeHiMs > view_.timeLoMs)) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, symbol = symbol_, tf = tfMs_, lo = view_.timeLoMs,
                                            hi = view_.timeHiMs] { c->setView(symbol, tf, lo, hi); },
                              Qt::QueuedConnection);
}

void HeatmapGpuLayer::postBudget() {
    if (!controller_) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, b = size_t(settings_.gpuCapBytes)] { c->setGpuBudget(b); },
                              Qt::QueuedConnection);
}

void HeatmapGpuLayer::onSnapshot() {
    if (!controller_) return;
    snapshot_ = controller_->latestSnapshot();
    if (!snapshot_) return;
    refreshPresets();
    noteLimits(); // the price scale may have changed
    emit snapshotChanged();
}

void HeatmapGpuLayer::onLive() {
    if (!controller_) return;
    live_ = controller_->latestLive();
    if (live_ && live_->version != lastLiveVersion_) {
        lastLiveVersion_ = live_->version;
        liveReceivedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    }
    emit liveChanged();
}

int64_t HeatmapGpuLayer::liveOpenEndMs() const {
    if (!live_ || live_->tfMs != tfMs_ || live_->symbol != symbol_) return 0;
    int64_t end = 0;
    for (const auto &source : live_->sources) end = std::max(end, source.openEndMs);
    return end;
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

bool HeatmapGpuLayer::prepareFrame(HeatmapTileNode::Frame &frame, const QRectF &rect, const ViewWindow &view) {
    if (!controller_) return false;
    // GUI thread blocked: take the freshest snapshot, pick the tick, hand both to
    // the node in this frame (no thread hop on a tick change).
    if (auto latest = controller_->latestSnapshot(); latest && latest != snapshot_) snapshot_ = std::move(latest);
    live_ = controller_->latestLive(); // pointer reads; the node uploads only a new version
    resolution_ = controller_->latestResolution();
    const int64_t tick = priceKnown_ ? chooseTick(view) : 0; // nothing to draw before the price view exists
    if (tick != tickUnits_) {
        tickUnits_ = tick;
        ++tickChanges_;
        sLog_Probe("heatmap.gpu.tick", "tf=" << tfMs_ << " units=" << tick << " mode=" << (manualMode_ ? "manual" : "auto"));
        QMetaObject::invokeMethod(this, [this] { emit tickChanged(); }, Qt::QueuedConnection);
    }
    if (tickUnits_ != postedTickUnits_ || manualMode_ != postedManual_) {
        postedTickUnits_ = tickUnits_;
        postedManual_ = manualMode_;
        const auto mode = manualMode_ ? TickMode::Manual : TickMode::Auto;
        QMetaObject::invokeMethod(controller_, [c = controller_, mode, units = tickUnits_] { c->setTickRequest(mode, units); },
                                  Qt::QueuedConnection);
    }
    frame.spans = snapshot_;
    frame.capacity = capacity_;
    frame.tfMs = tfMs_;
    frame.tickUnits = tickUnits_;
    frame.view = view;
    frame.rect = rect;
    frame.uploadBudgetBytes = settings_.uploadBudgetBytes;
    frame.gpuCapBytes = settings_.gpuCapBytes;
    frame.style = style_;
    frame.palette = palette_;
    frame.crossfadeMs = settings_.crossfadeMs;
    frame.live = live_;
    renderedSerial_ = viewSerial_;
    return true;
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
                    {"snapshotVersion", qint64(snapshot_ ? snapshot_->version : 0)}};
    if (controllerStats_->valid) {
        const auto &c = controllerStats_->stats;
        out["controllerStats"] = QJsonObject{{"publications", qint64(c.publications)},
                                             {"admissions", qint64(c.admissions)},
                                             {"evictions", qint64(c.evictions)},
                                             {"suppressed", qint64(c.suppressed)},
                                             {"refused", qint64(c.refused)},
                                             {"committedBytes", qint64(c.committedBytes)},
                                             {"livePublications", qint64(c.livePublications)},
                                             {"liveComposeMs", c.liveComposeMs},
                                             {"liveIntervalMs", c.liveIntervalMs}};
    } else {
        out["controllerStats"] = QJsonValue(QJsonValue::Null);
    }
    // Refresh the controller's stats for the next call (they belong to its thread).
    if (controller_)
        QMetaObject::invokeMethod(controller_, [c = controller_, stats = controllerStats_] {
            const auto value = c->stats();
            QMetaObject::invokeMethod(QCoreApplication::instance(), [stats, value] {
                stats->stats = value;
                stats->valid = true;
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    return out;
}
} // namespace heatmap::gpu
