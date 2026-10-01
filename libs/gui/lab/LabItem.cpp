#include "LabItem.hpp"
#include "LabSources.hpp"
#include "SentinelLogging.hpp"
#include "heatmap/HeatmapSpanPlanner.hpp"
#include "render/heatmap/HeatmapGpuSelfTest.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QQuickWindow>
#include <QSettings>
#include <QTimeZone>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace lab {
namespace {
QString money(double price) { return QStringLiteral("$") + QString::number(price, 'g', 12); }
QString utc(int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("MM-dd HH:mm"));
}
QString utcSeconds(int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss"));
}
bool inView(const heatmap::ColumnResolution &column, int64_t tfMs, const heatmap::gpu::ViewWindow &view) {
    return double(column.startMs) < view.timeHiMs && double(column.startMs + tfMs) > view.timeLoMs;
}
} // namespace

LabItem::LabItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    heatmap::gpu::prewarmPrecisionSelfTest(); // fixture builds on a worker, not in prepare()
    launched_.start();
}
LabItem::~LabItem() {
    if (controller_) {
        QObject::disconnect(controller_, nullptr, this, nullptr);
        LabData::instance().destroyController(controller_);
    }
}

void LabItem::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange) {
        QObject::disconnect(frameConnection_);
        if (value.window) {
            // The lab redraws continuously so frame telemetry stays live; a
            // redraw that does not move the view costs one draw, no compute.
            frameConnection_ = connect(value.window, &QQuickWindow::frameSwapped, this, [this] {
                if (lastFrame_.isValid()) frameMs_ = lastFrame_.nsecsElapsed() / 1e6;
                lastFrame_.start();
                if (firstFrameMs_ == 0 && tileStats_->readySlots.load() != 0)
                    firstFrameMs_ = launched_.nsecsElapsed() / 1e6;
                update();
            }, Qt::QueuedConnection);
        }
    }
    QQuickItem::itemChange(change, value);
}

// ------------------------------------------------------------------ frame
int64_t LabItem::chooseTick() {
    if (!snapshot_ || snapshot_->tfMs != tfMs()) return tickUnits_; // the new tf has nothing built yet
    if (manualMode_) {
        const int64_t units = heatmap::toUnits(manualTick_, priceScale());
        return heatmap::isPresetUnits(units) ? units : tickUnits_;
    }
    // Auto: the history summary merged with the live window's columns (S5L-b
    // latestResolution(); a live revision never replaces the SpanSet).
    const heatmap::ResolutionSummary &summary =
        resolution_ && resolution_->tfMs == tfMs() ? *resolution_ : snapshot_->resolution;
    const double heightPx = height() * devicePixelRatio();
    TickKey key{snapshot_->version, &summary, view_, heightPx, minRowPx_, hysteresis_, autoUnits_};
    if (key == tickKey_) return autoUnits_ > 0 ? autoUnits_ : tickUnits_;
    const int64_t units = heatmap::autoTickUnits(summary, autoUnits_, view_.timeLoMs, view_.timeHiMs,
                                                 view_.priceLo, view_.priceHi, heightPx, {minRowPx_, hysteresis_});
    if (units > 0) autoUnits_ = units;
    key.current = autoUnits_;
    tickKey_ = key;
    return autoUnits_ > 0 ? autoUnits_ : tickUnits_;
}

QSGNode *LabItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = old ? static_cast<heatmap::gpu::HeatmapTileNode *>(old) : new heatmap::gpu::HeatmapTileNode(tileStats_);
    // GUI thread blocked: take the freshest snapshot, pick the tick, hand both to
    // the node in this frame (no thread hop on a tick change).
    if (controller_) {
        if (auto latest = controller_->latestSnapshot(); latest && latest != snapshot_) snapshot_ = std::move(latest);
        live_ = controller_->latestLive(); // pointer reads; the node uploads only a new version
        resolution_ = controller_->latestResolution();
    }
    const int64_t tick = priceKnown_ ? chooseTick() : 0; // nothing to draw before the price view exists
    if (tick != tickUnits_) {
        tickUnits_ = tick;
        ++tickChanges_;
        sLog_Probe("lab.tick", "tf=" << tfMs() << " units=" << tick << " mode=" << (manualMode_ ? "manual" : "auto"));
        QMetaObject::invokeMethod(this, [this] { emit tickChanged(); }, Qt::QueuedConnection);
    }
    if (controller_ && (tickUnits_ != postedTickUnits_ || manualMode_ != postedManual_)) {
        postedTickUnits_ = tickUnits_;
        postedManual_ = manualMode_;
        const auto mode = manualMode_ ? heatmap::TickMode::Manual : heatmap::TickMode::Auto;
        QMetaObject::invokeMethod(controller_, [c = controller_, mode, units = tickUnits_] { c->setTickRequest(mode, units); },
                                  Qt::QueuedConnection);
    }
    heatmap::gpu::HeatmapTileNode::Frame frame;
    frame.spans = snapshot_;
    frame.capacity = capacity_;
    frame.tfMs = tfMs();
    frame.tickUnits = tickUnits_;
    frame.view = view_;
    frame.rect = QRectF(0, 0, width(), height());
    frame.uploadBudgetBytes = uploadBudget_;
    frame.gpuCapBytes = gpuCap_;
    frame.crossfadeMs = crossfade_ ? kCrossfadeMs : 0;
    frame.live = live_;
    node->setFrame(std::move(frame));
    renderedSerial_ = viewSerial_;
    return node;
}

void LabItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickItem::geometryChange(next, previous);
    clampView();
    viewChanged();
}

double LabItem::devicePixelRatio() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

bool LabItem::settled() const {
    if (!loaded_ || !priceKnown_ || !snapshot_ || snapshot_->tfMs != tfMs() || tickUnits_ <= 0) return false;
    if (renderedSerial_ != viewSerial_) return false; // the last frame showed an older view
    return tileStats_->complete.load() && tileStats_->drawnTickUnits.load() == tickUnits_ &&
           tileStats_->drawnTfMs.load() == tfMs();
}

// ------------------------------------------------------------------ data
void LabItem::loadReal(int hours) {
    if (hours < 1 || hours > 24 * 30) return;
    hours_ = hours;
    if (!controller_) {
        auto &data = LabData::instance();
        controller_ = data.createController(gpuCap_);
        capacity_ = controller_->capacity();
        connect(controller_, &heatmap::HeatmapSourceController::snapshotChanged, this, [this] { onSnapshot(); },
                Qt::QueuedConnection);
        connect(controller_, &heatmap::HeatmapSourceController::liveChanged, this, [this] { onLive(); },
                Qt::QueuedConnection);
        connect(controller_, &heatmap::HeatmapSourceController::buildFailed, this, [this](const QString &message) {
            status_ = QStringLiteral("Span build failed: %1").arg(message);
            emit statusChanged();
        }, Qt::QueuedConnection);
    }
    status_ = QStringLiteral("Waiting for the recording's availability...");
    emit statusChanged();
    // The transport scans availability once on start: poll until it is known.
    auto *poll = new QTimer(this);
    poll->setInterval(20);
    connect(poll, &QTimer::timeout, this, [this, poll] {
        const auto available = LabData::instance().availability();
        if (!available) return;
        poll->deleteLater();
        int64_t end = 0, oldest = INT64_MAX;
        for (const auto &source : available->sources)
            for (const auto &level : source.levels) {
                end = std::max(end, level.committedThroughMs);
                oldest = std::min(oldest, level.oldestMs);
            }
        if (end <= 0) {
            status_ = QStringLiteral("Timeframe unavailable: no recording for %1").arg(QString::fromLatin1(kSymbol));
            emit statusChanged();
            return;
        }
        loaded_ = true;
        if (pendingView_) {
            view_ = *pendingView_;
            pendingView_.reset();
            priceKnown_ = true;
        } else {
            const int64_t tf = tfMs();
            // Server mode: the open minute (the live edge) is in view too.
            const int64_t edge = followLive_ ? end + heatmap::kMinuteMs : end;
            const double right = double(recording::floorDiv(edge + tf - 1, tf) * tf) + (followLive_ ? 2.0 * tf : 0.0);
            const double span = std::min(initialTimeSpanMs_ > 0 ? initialTimeSpanMs_ : double(hours_) * heatmap::kHourMs,
                                         heatmap::maxTimeSpanMs(std::max(1.0, width()) * devicePixelRatio(), tf));
            view_.timeHiMs = right;
            view_.timeLoMs = right - span;
            view_.priceLo = 0;
            view_.priceHi = 1; // the recent mid is known once the first spans are built
        }
        status_ = QStringLiteral("%1 · %2 h of recording").arg(QString::fromLatin1(kSymbol))
                      .arg(double(end - oldest) / heatmap::kHourMs, 0, 'f', 1);
        emit statusChanged();
        restoreTick();
        clampView();
        viewChanged();
    });
    poll->start();
}

// Server mode: keep the newest live bucket in view, a little in from the right
// edge, until the user pans away.
void LabItem::onLive() {
    if (!controller_) return;
    live_ = controller_->latestLive();
    update();
    if (!followLive_ || !live_ || live_->tfMs != tfMs() || !loaded_) return;
    int64_t end = 0;
    for (const auto &source : live_->sources) end = std::max(end, source.openEndMs);
    if (end <= 0) return;
    const double tf = double(tfMs()), span = view_.timeHiMs - view_.timeLoMs;
    const double liveEnd = std::ceil(double(end) / tf) * tf;
    if (liveEnd + 0.5 * tf <= view_.timeHiMs) return;
    const double shift = liveEnd + std::max(2.0 * tf, 0.08 * span) - view_.timeHiMs;
    view_.timeLoMs += shift;
    view_.timeHiMs += shift;
    viewChanged();
}

void LabItem::onSnapshot() {
    if (!controller_) return;
    snapshot_ = controller_->latestSnapshot();
    if (!snapshot_) return;
    if (!priceKnown_) initialisePrice();
    refreshPresets();
    if (initialRowPx_ > 0 && priceKnown_ && commonTickInView() > 0) {
        const double px = initialRowPx_;
        initialRowPx_ = 0;
        zoomToRowPx(px);
    }
    if (showBandEdges_) emit bandEdgesChanged();
    update();
}

// The initial price view: +-2 % around the median mid of the newest decoded
// minutes of the view (the chunk store holds them once the spans are built).
void LabItem::initialisePrice() {
    const auto &set = *snapshot_;
    const heatmap::SpanSourceBuild *best = nullptr;
    int64_t bestTile = INT64_MIN;
    for (const auto &span : set.spans) {
        if (span.id.tfMs != tfMs() || span.rank.tier != heatmap::SpanTier::Visible) continue;
        for (const auto &source : span.sources)
            if (source.build && (span.id.tile > bestTile || (span.id.tile == bestTile && best &&
                                                             source.build->commonUnits < best->commonUnits))) {
                best = source.build.get();
                bestTile = span.id.tile;
            }
    }
    if (!best) return;
    double price = 0;
    auto &store = LabData::instance().store();
    for (auto it = best->key.generations.rbegin(); it != best->key.generations.rend() && !(price > 0); ++it)
        if (const auto chunk = store.cached({it->symbol, it->source, it->levelMs, it->startMs}))
            price = medianRecentPrice(*chunk->columns);
    if (!(price > 0)) {
        // No decoded minutes left: the centre of the source's coverage.
        for (const auto &column : best->resolution.columns)
            for (const auto &source : column.sources)
                if (!source.bands.empty())
                    price = heatmap::fromUnits((source.bands.front().lo + source.bands.back().end) / 2, set.priceScale);
    }
    if (!(price > 0)) return;
    view_.priceLo = initialPriceSpan_ > 0 ? price - initialPriceSpan_ / 2 : price * 0.98;
    view_.priceHi = initialPriceSpan_ > 0 ? price + initialPriceSpan_ / 2 : price * 1.02;
    priceKnown_ = true;
    clampView();
    viewChanged();
}

void LabItem::refreshPresets() {
    if (!snapshot_ || snapshot_->tfMs != tfMs()) return;
    int64_t finest = 0;
    for (const auto &column : snapshot_->resolution.columns)
        for (const auto &source : column.sources)
            if (source.commonUnits > 0) finest = finest ? std::min(finest, source.commonUnits) : source.commonUnits;
    if (!finest) return;
    QVariantList offered;
    for (const int64_t units : heatmap::manualPresetUnits(snapshot_->resolution, finest * 1000))
        offered.push_back(heatmap::fromUnits(units, priceScale()));
    if (offered != offeredTicks_) {
        offeredTicks_ = offered;
        emit presetsChanged();
    }
}

void LabItem::postView() {
    if (!controller_ || !loaded_) return;
    QMetaObject::invokeMethod(controller_, [c = controller_, symbol = symbol(), tf = tfMs(), lo = view_.timeLoMs,
                                            hi = view_.timeHiMs] { c->setView(symbol, tf, lo, hi); },
                              Qt::QueuedConnection);
}

void LabItem::viewChanged() {
    ++viewSerial_;
    postView();
    if (showBandEdges_) emit bandEdgesChanged();
    update();
}

void LabItem::setView(const heatmap::gpu::ViewWindow &view) {
    view_ = view;
    priceKnown_ = priceKnown_ || view.priceHi > view.priceLo + 1e-9;
    clampView();
    viewChanged();
}

void LabItem::setGpuCapBytes(uint64_t bytes) {
    gpuCap_ = std::max<uint64_t>(bytes, 1);
    if (controller_)
        QMetaObject::invokeMethod(controller_, [c = controller_, b = gpuCap_] { c->setGpuBudget(size_t(b)); },
                                  Qt::QueuedConnection);
    update();
}

// ------------------------------------------------------------------ tick policy
// Spec rules 1, 2 and 9: time zoom-out stops at one column per physical pixel;
// in Manual, price zoom-out stops at one row per physical pixel.
void LabItem::clampView() {
    if (!(width() > 0) || !(height() > 0)) return;
    const double dpr = devicePixelRatio();
    heatmap::clampSpan(view_.timeLoMs, view_.timeHiMs, heatmap::maxTimeSpanMs(width() * dpr, tfMs()));
    if (manualMode_ && priceKnown_)
        heatmap::clampSpan(view_.priceLo, view_.priceHi, heatmap::maxManualPriceSpan(height() * dpr, manualTick_));
}

void LabItem::loadTickMemory() {
    QSettings settings(QStringLiteral("Sentinel"), QStringLiteral("sentinel-lab"));
    settings.beginGroup(QStringLiteral("manualTick"));
    for (const QString &key : settings.allKeys()) { // "<symbol>/<tfMinutes>" = price units
        const auto parts = key.split('/');
        bool okTf = false, okUnits = false;
        const qlonglong minutes = parts.size() == 2 ? parts[1].toLongLong(&okTf) : 0;
        const qlonglong units = settings.value(key).toLongLong(&okUnits);
        if (okTf && okUnits) tickMemory_.set(parts[0].toStdString(), minutes * 60'000, units);
    }
}

void LabItem::setPersistTickMemory(bool persist) {
    persistTickMemory_ = persist;
    if (persist) loadTickMemory();
}

void LabItem::rememberTick() {
    const int64_t units = heatmap::toUnits(manualTick_, priceScale());
    if (!tickMemory_.set(symbol(), tfMs(), units) || !persistTickMemory_) return;
    QSettings settings(QStringLiteral("Sentinel"), QStringLiteral("sentinel-lab"));
    settings.setValue(QStringLiteral("manualTick/%1/%2").arg(QString::fromStdString(symbol())).arg(timeframeMinutes_),
                      qlonglong(units));
}

// Manual: the tick remembered for (symbol, timeframe) wins; with none, keep the
// locked tick and remember it for this (symbol, timeframe).
void LabItem::restoreTick() {
    if (!manualMode_) return;
    if (explicitTickPending_) {
        explicitTickPending_ = false;
        rememberTick();
    } else if (const auto units = tickMemory_.get(symbol(), tfMs())) {
        const double tick = heatmap::fromUnits(*units, priceScale());
        if (tick != manualTick_) { manualTick_ = tick; emit tickChanged(); }
    } else {
        if (!heatmap::isPresetUnits(heatmap::toUnits(manualTick_, priceScale()))) {
            manualTick_ = fallbackManualTick();
            emit tickChanged();
        }
        rememberTick();
    }
    clampView();
}

// The tick drawn now if it is a preset, else the finest preset the loaded data builds.
double LabItem::fallbackManualTick() const {
    if (heatmap::isPresetUnits(tickUnits_)) return heatmap::fromUnits(tickUnits_, priceScale());
    if (!offeredTicks_.isEmpty()) return offeredTicks_.front().toDouble();
    return 1.0;
}

void LabItem::setTimeframeMinutes(int minutes) {
    if (minutes < 1 || minutes > 1440 || timeframeMinutes_ == minutes) return;
    timeframeMinutes_ = minutes;
    autoUnits_ = 0; // Auto evaluates fresh on the new timeframe's data
    restoreTick(); // Manual: this timeframe's remembered tick
    clampView();
    emit timeframeChanged();
    // The old picture keeps drawing (node hold) until the new timeframe's spans are ready.
    viewChanged();
}

void LabItem::setManualMode(bool manual) {
    if (manualMode_ == manual) return;
    manualMode_ = manual;
    if (manual) {
        const auto remembered = tickMemory_.get(symbol(), tfMs());
        manualTick_ = remembered ? heatmap::fromUnits(*remembered, priceScale()) : fallbackManualTick();
        rememberTick();
    }
    autoUnits_ = 0; // back to Auto: the rule resumes from the current zoom
    clampView();
    emit tickChanged();
    viewChanged();
}

void LabItem::setManualTick(double tick) {
    if (!std::isfinite(tick) || !(tick > 0) || !heatmap::isPresetUnits(heatmap::toUnits(tick, priceScale()))) {
        status_ = QStringLiteral("Manual tick must be a preset {1, 2, 2.5, 5} x 10^k: %1").arg(tick);
        emit statusChanged();
        emit tickChanged();
        return;
    }
    const bool changed = !manualMode_ || tick != manualTick_;
    manualMode_ = true;
    manualTick_ = tick;
    if (!loaded_) explicitTickPending_ = true;
    else rememberTick();
    clampView();
    if (changed) emit tickChanged();
    viewChanged();
}

void LabItem::setHysteresis(double h) {
    if (!std::isfinite(h) || h < 0 || h > 0.9 || h == hysteresis_) return;
    hysteresis_ = h;
    emit tickChanged();
    viewChanged();
}
void LabItem::setMinRowPx(double px) {
    if (!std::isfinite(px) || px < 0.5 || px > 32 || px == minRowPx_) return;
    minRowPx_ = px;
    emit tickChanged();
    viewChanged();
}
void LabItem::setCrossfade(bool enabled) {
    if (crossfade_ == enabled) return;
    crossfade_ = enabled;
    emit tickChanged();
    update();
}
void LabItem::setShowBandEdges(bool show) {
    if (showBandEdges_ == show) return;
    showBandEdges_ = show;
    emit bandEdgesChanged();
}

// ------------------------------------------------------------------ input
void LabItem::pan(double dx, double dy) {
    if (!hasContent() || width() <= 0 || height() <= 0) return;
    if (dx != 0) followLive_ = false;
    const double dt = dx / width() * (view_.timeHiMs - view_.timeLoMs);
    const double dp = dy / height() * (view_.priceHi - view_.priceLo);
    view_.timeLoMs -= dt; view_.timeHiMs -= dt;
    view_.priceLo += dp; view_.priceHi += dp;
    viewChanged();
}

// The finest common tick of any column in view (a zoom reference; 0 when none).
double LabItem::commonTickInView() const {
    if (!snapshot_ || snapshot_->tfMs != tfMs()) return 0;
    int64_t finest = 0;
    for (const auto &column : snapshot_->resolution.columns) {
        if (!inView(column, snapshot_->tfMs, view_)) continue;
        for (const auto &source : column.sources)
            if (source.state == heatmap::BucketState::Present && source.commonUnits > 0 && !source.bands.empty())
                finest = finest ? std::min(finest, source.commonUnits) : source.commonUnits;
    }
    return finest ? heatmap::fromUnits(finest, priceScale()) : 0;
}

void LabItem::zoom(double steps, bool priceOnly, double anchorX, double anchorY) {
    if (!hasContent()) return;
    const double factor = std::exp(-steps * 0.12);
    const double dpr = devicePixelRatio();
    if (!priceOnly) {
        // The timeframe never changes on zoom; zoom-out stops at one column per pixel.
        const double span = view_.timeHiMs - view_.timeLoMs;
        const double maximum = heatmap::maxTimeSpanMs(std::max(1.0, width()) * dpr, tfMs());
        const double wanted = std::min(std::max(60'000.0, span * factor), maximum);
        const double a = std::clamp(anchorX, 0.0, 1.0);
        const double center = view_.timeLoMs + a * span;
        view_.timeLoMs = center - a * wanted;
        view_.timeHiMs = view_.timeLoMs + wanted;
    }
    const double priceSpan = view_.priceHi - view_.priceLo;
    const double finest = std::max(commonTickInView(), 0.01);
    double desired = std::max(finest * 4, priceSpan * factor);
    if (manualMode_) {
        const double maximum = heatmap::maxManualPriceSpan(std::max(1.0, height()) * dpr, manualTick_);
        if (maximum > 0) desired = std::min(desired, maximum);
    }
    const double a = 1.0 - std::clamp(anchorY, 0.0, 1.0);
    const double center = view_.priceLo + a * priceSpan;
    view_.priceLo = center - a * desired;
    view_.priceHi = view_.priceLo + desired;
    viewChanged();
}

void LabItem::wheelZoom(double angleX, double angleY, bool priceOnly, double anchorX, double anchorY) {
    double delta = angleY;
    if (priceOnly && delta == 0) delta = angleX; // macOS: Shift turns the vertical wheel horizontal
    if (delta == 0 || !std::isfinite(delta)) return;
    zoom(delta / 120.0, priceOnly, anchorX, anchorY);
}

void LabItem::zoomToRowPx(double px) {
    if (!hasContent() || !(px > 0) || !std::isfinite(px) || !(height() > 0)) return;
    const double common = commonTickInView();
    if (!(common > 0)) return;
    const double span = height() * devicePixelRatio() * common / px;
    const double center = (view_.priceLo + view_.priceHi) * 0.5;
    view_.priceLo = center - span * 0.5;
    view_.priceHi = center + span * 0.5;
    clampView();
    viewChanged();
}

// ------------------------------------------------------------------ indicators
QString LabItem::resolutionIndicator() const {
    if (!snapshot_ || snapshot_->tfMs != tfMs() || tickUnits_ <= 0) return {};
    const auto ranges = heatmap::veiledRanges(snapshot_->resolution, tickUnits_, view_.timeLoMs, view_.timeHiMs,
                                              view_.priceLo, view_.priceHi);
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
        .arg(money(heatmap::fromUnits(tickUnits_, scale)), money(heatmap::fromUnits(lo, scale)),
             money(heatmap::fromUnits(hi, scale)), utc(start), utc(end));
}

QVariantList LabItem::bandEdges() const {
    QVariantList out;
    if (!snapshot_ || snapshot_->tfMs != tfMs() || !(width() > 0) || !(height() > 0)) return out;
    const auto &summary = snapshot_->resolution;
    // The finest source in view (a band source; no code knows its name).
    std::string finest;
    int64_t finestUnits = 0;
    for (const auto &column : summary.columns) {
        if (!inView(column, summary.tfMs, view_)) continue;
        for (const auto &source : column.sources)
            if (source.commonUnits > 0 && !source.bands.empty() && (!finestUnits || source.commonUnits < finestUnits)) {
                finestUnits = source.commonUnits;
                finest = source.source;
            }
    }
    if (finest.empty()) return out;
    const double tSpan = view_.timeHiMs - view_.timeLoMs, pSpan = view_.priceHi - view_.priceLo;
    auto x = [&](double ms) { return (ms - view_.timeLoMs) / tSpan * width(); };
    auto y = [&](int64_t units) {
        const double price = heatmap::fromUnits(units, summary.priceScale);
        return std::clamp((view_.priceHi - price) / pSpan * height(), -1.0, height() + 1);
    };
    int64_t runStart = 0, runEnd = 0, runLo = 0, runHi = 0;
    bool open = false;
    auto flush = [&] {
        if (open) out.push_back(QVariantList{x(double(runStart)), x(double(runEnd)), y(runHi), y(runLo)});
        open = false;
    };
    for (const auto &column : summary.columns) {
        if (!inView(column, summary.tfMs, view_)) continue;
        const heatmap::SourceResolution *band = nullptr;
        for (const auto &source : column.sources)
            if (source.source == finest && !source.bands.empty()) band = &source;
        if (!band) { flush(); continue; }
        const int64_t lo = band->bands.front().lo, hi = band->bands.back().end;
        if (open && lo == runLo && hi == runHi && column.startMs == runEnd) {
            runEnd += summary.tfMs;
            continue;
        }
        flush();
        runStart = column.startMs;
        runEnd = column.startMs + summary.tfMs;
        runLo = lo;
        runHi = hi;
        open = true;
    }
    flush();
    return out;
}

bool LabItem::saveScreenshot(const QString &path) {
    if (!window() || !hasContent()) return false;
    const QString target = path.isEmpty() ?
        QStringLiteral("screenshots/lab-%1.png").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")) : path;
    const QFileInfo file(target);
    if (!labOutputAllowed(target)) return false; // before mkpath and again before the write
    QDir().mkpath(file.absolutePath());
    if (!labOutputAllowed(target)) return false;
    return window()->grabWindow().save(target, "PNG");
}

QVariantMap LabItem::metrics() const {
    const auto &s = *tileStats_;
    const double tick = heatmap::fromUnits(tickUnits_, priceScale());
    const double heightPx = height() * devicePixelRatio();
    const auto data = LabData::instance().stats();
    QVariantMap m{{"fps", frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0}, {"frameMs", frameMs_},
                  {"firstFrameMs", firstFrameMs_}, {"timeframeMinutes", timeframeMinutes_},
                  {"mode", manualMode_ ? QStringLiteral("manual") : QStringLiteral("auto")},
                  {"tick", tick}, {"manualTick", manualTick_}, {"hysteresis", hysteresis_}, {"minRowPx", minRowPx_},
                  {"crossfadeMs", crossfade_ ? kCrossfadeMs : 0.0}, {"crossfading", s.crossfading.load()},
                  {"holding", s.holding.load()}, {"commonTick", commonTickInView()},
                  {"tickChanges", qulonglong(tickChanges_)},
                  {"rowPx", tick > 0 && view_.priceHi > view_.priceLo ? tick * heightPx / (view_.priceHi - view_.priceLo) : 0.0},
                  {"priceSpan", view_.priceHi - view_.priceLo}, {"timeSpanMin", (view_.timeHiMs - view_.timeLoMs) / 60'000.0},
                  {"priceLo", view_.priceLo}, {"priceHi", view_.priceHi},
                  {"indicator", resolutionIndicator()}, {"settled", settled()},
                  {"lastBinMs", s.lastBinMs.load()}, {"prepareMs", s.prepareMs.load()},
                  {"gpuFrameMs", s.gpuFrameMs.load()},
                  {"gpuBytes", qulonglong(s.gpuBytes.load())}, {"residentBytes", qulonglong(s.residentBytes.load())},
                  {"sourceBytes", qulonglong(s.sourceBytes.load())}, {"binBytes", qulonglong(s.binBytes.load())},
                  {"residentSources", qulonglong(s.residentSources.load())},
                  {"sourcesUploaded", qulonglong(s.sourcesUploaded.load())},
                  {"uploadBytes", qulonglong(s.uploadBytes.load())}, {"binPasses", qulonglong(s.binPasses.load())},
                  {"fillPasses", qulonglong(s.fillPasses.load())}, {"binsMade", qulonglong(s.binsMade.load())},
                  {"rebins", qulonglong(s.rebins.load())}, {"evictions", qulonglong(s.evictions.load())},
                  {"missingReports", qulonglong(s.missingReports.load())}, {"reports", qulonglong(s.reports.load())},
                  {"slots", s.slotCount.load()}, {"readySlots", s.readySlots.load()},
                  {"fallbackSlots", s.fallbackSlots.load()}, {"partialSlots", s.partialSlots.load()},
                  {"loadingSlots", s.loadingSlots.load()}, {"refusedSlots", s.refusedSlots.load()},
                  {"missingDraws", qulonglong(s.missingDraws.load())}, {"errors", qulonglong(s.errors.load())},
                  {"kernel", s.preciseKernel.load() ? QStringLiteral("precise") : QStringLiteral("fast")},
                  {"refusedSpans", snapshot_ ? int(snapshot_->refused.size()) : 0},
                  {"refusedBytes", snapshot_ ? qulonglong(snapshot_->refusedBytes) : 0},
                  {"snapshotVersion", snapshot_ ? qulonglong(snapshot_->version) : 0},
                  {"chunkBytes", qulonglong(data.store.bytes)}, {"chunkEntries", qulonglong(data.store.entries)},
                  {"chunkLoads", qulonglong(data.store.loads)}, {"chunkEvictions", qulonglong(data.store.evictions)},
                  {"chunkRevisions", qulonglong(data.store.revisions)},
                  {"fetchRequests", qulonglong(data.fetcher.requests)},
                  {"fetchedChunks", qulonglong(data.fetcher.requestedChunks)},
                  {"spanBuilds", qulonglong(data.cache.builds)}, {"spanCacheHits", qulonglong(data.cache.hits)},
                  {"spanLiveBytes", qlonglong(data.cache.liveBytes)},
                  {"spanCacheBytes", qulonglong(data.cache.bytes)}, // LRU (claimed or not)
                  {"spanClaimedBytes", qulonglong(data.cache.claimedBytes)},
                  {"spanReservedBytes", qulonglong(data.cache.reservedBytes)},
                  {"chunkWantedBytes", qulonglong(data.store.wantedBytes)},
                  {"cpuCommittedBytes", qulonglong(data.committedCpuBytes)},
                  {"footprintBytes", qulonglong(processFootprintBytes())},
                  {"connection", LabData::instance().connectionText()},
                  {"liveVersion", qulonglong(s.liveVersion.load())},
                  {"livePublished", live_ ? qulonglong(live_->version) : 0},
                  {"liveUploads", qulonglong(s.liveUploads.load())},
                  {"liveBinPasses", qulonglong(s.liveBinPasses.load())},
                  {"liveBufferCreations", qulonglong(s.liveBufferCreations.load())},
                  {"liveSets", s.liveSets.load()},
                  {"livePublishToDrawMs", s.livePublishToDrawMs.load()},
                  {"liveDataAgeMs", s.liveDataAgeMs.load()},
                  {"liveL", s.liveStartMs.load() ? utcSeconds(s.liveStartMs.load()) : QString()},
                  {"liveEnd", s.liveEndMs.load() ? utcSeconds(s.liveEndMs.load()) : QString()},
                  {"liveFrom", s.liveDrawFromMs.load() ? utcSeconds(s.liveDrawFromMs.load()) : QString()},
                  {"liveE", s.liveEdgeCompleteMs.load() ? utcSeconds(s.liveEdgeCompleteMs.load()) : QString()},
                  {"liveSpanMin", double(s.liveEndMs.load() - s.liveStartMs.load()) / 60'000.0}};
    {
        // Latency percentiles over every live version drawn so far.
        auto samples = s.liveSamples();
        auto percentile = [](std::vector<double> v, double q) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
        };
        std::vector<double> publish, age;
        for (const auto &[p, a] : samples) { publish.push_back(p); age.push_back(a); }
        m["liveSamples"] = qulonglong(samples.size());
        m["livePublishP50"] = percentile(publish, 0.5);
        m["livePublishP95"] = percentile(publish, 0.95);
        m["liveAgeP50"] = percentile(age, 0.5);
        m["liveAgeP95"] = percentile(age, 0.95);
    }
    // E4: the finest source's band in the newest column in view (price).
    if (snapshot_ && snapshot_->tfMs == tfMs()) {
        int64_t finest = 0, lo = 0, hi = 0;
        for (const auto &column : snapshot_->resolution.columns) {
            if (!inView(column, snapshot_->tfMs, view_)) continue;
            for (const auto &source : column.sources)
                if (source.commonUnits > 0 && !source.bands.empty() && (!finest || source.commonUnits <= finest)) {
                    finest = source.commonUnits;
                    lo = source.bands.front().lo;
                    hi = source.bands.back().end;
                }
        }
        if (finest) {
            m["fineBandLo"] = heatmap::fromUnits(lo, snapshot_->priceScale);
            m["fineBandHi"] = heatmap::fromUnits(hi, snapshot_->priceScale);
        }
    }
    if (controllerStats_->valid) {
        const auto &c = controllerStats_->stats;
        m.insert({{"publications", qulonglong(c.publications)}, {"admissions", qulonglong(c.admissions)},
                  {"controllerEvictions", qulonglong(c.evictions)}, {"releasedImages", qulonglong(c.releasedImages)},
                  {"suppressed", qulonglong(c.suppressed)}, {"committedBytes", qulonglong(c.committedBytes)},
                  {"livePublications", qulonglong(c.livePublications)}, {"liveComposeMs", c.liveComposeMs},
                  {"liveIntervalMs", c.liveIntervalMs}});
    }
    // Refresh the controller's stats for the next call (they belong to its thread).
    if (controller_)
        QMetaObject::invokeMethod(controller_, [c = controller_, out = controllerStats_] {
            const auto stats = c->stats();
            QMetaObject::invokeMethod(QCoreApplication::instance(), [out, stats] {
                out->stats = stats;
                out->valid = true;
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    return m;
}
} // namespace lab
