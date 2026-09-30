#include "LabItem.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "render/heatmap/HeatmapGpuSelfTest.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QQuickWindow>
#include <QSettings>
#include <QThreadPool>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <exception>

namespace lab {
namespace {
constexpr uint64_t kUploadBudgetBytes = 2ull << 20; // plan default: 2 MiB per frame
QString unavailableStatus(QString detail) { return QStringLiteral("Timeframe unavailable: %1").arg(detail.trimmed()); }
QString money(double price) { return QStringLiteral("$") + QString::number(price, 'g', 12); }
QString utc(int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("MM-dd HH:mm"));
}
} // namespace

LabItem::LabItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    heatmap::gpu::prewarmPrecisionSelfTest(); // fixture builds on a worker, not in prepare()
    launched_.start();
}
LabItem::~LabItem() { loadGeneration_->fetch_add(1); }

void LabItem::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange) {
        QObject::disconnect(frameConnection_);
        if (value.window) {
            // The lab redraws continuously so frame telemetry stays live; a
            // redraw that does not move the view costs one draw, no compute.
            frameConnection_ = connect(value.window, &QQuickWindow::frameSwapped, this, [this] {
                if (lastFrame_.isValid()) frameMs_ = lastFrame_.nsecsElapsed() / 1e6;
                lastFrame_.start();
                if (firstFrameMs_ == 0 && stats_->drawnSourceId.load() != 0) firstFrameMs_ = launched_.nsecsElapsed() / 1e6;
                update();
            }, Qt::QueuedConnection);
        }
    }
    QQuickItem::itemChange(change, value);
}

QSGNode *LabItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = old ? static_cast<heatmap::gpu::HeatmapRenderNode *>(old)
                     : new heatmap::gpu::HeatmapRenderNode(stats_);
    heatmap::gpu::HeatmapRenderNode::Frame frame;
    frame.source = source_.gpu;
    frame.view = view_;
    // The node picks the tick for the source it actually draws (Auto: commonTick
    // of the active source's data in view), so a pending source cannot veil it.
    frame.tick.mode = manualMode_ ? heatmap::TickMode::Manual : heatmap::TickMode::Auto;
    frame.tick.manualTick = manualTick_;
    frame.tick.minRowPx = minRowPx_;
    frame.tick.hysteresis = hysteresis_;
    frame.tick.crossfadeMs = crossfade_ ? kCrossfadeMs : 0;
    frame.tick.heightPx = height() * devicePixelRatio();
    frame.rect = QRectF(0, 0, width(), height());
    frame.uploadBudgetBytes = kUploadBudgetBytes;
    node->setFrame(std::move(frame));
    return node;
}

void LabItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickItem::geometryChange(next, previous);
    clampView();
    update();
}

double LabItem::devicePixelRatio() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

// Spec rules 1, 2 and 9: time zoom-out stops at one column per physical pixel;
// in Manual, price zoom-out stops at one row per physical pixel.
void LabItem::clampView() {
    if (!(width() > 0) || !(height() > 0)) return;
    const double dpr = devicePixelRatio();
    heatmap::clampSpan(view_.timeLoMs, view_.timeHiMs, heatmap::maxTimeSpanMs(width() * dpr, tfMs()));
    if (manualMode_) heatmap::clampSpan(view_.priceLo, view_.priceHi, heatmap::maxManualPriceSpan(height() * dpr, manualTick_));
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
    const auto sym = symbol();
    const int64_t units = heatmap::toUnits(manualTick_, priceScale());
    if (sym.empty() || !tickMemory_.set(sym, tfMs(), units) || !persistTickMemory_) return;
    QSettings settings(QStringLiteral("Sentinel"), QStringLiteral("sentinel-lab"));
    settings.setValue(QStringLiteral("manualTick/%1/%2").arg(QString::fromStdString(sym)).arg(timeframeMinutes_),
                      qlonglong(units));
}

// Manual: the tick remembered for (symbol, timeframe) wins; with none, keep the
// locked tick and remember it for this (symbol, timeframe).
void LabItem::restoreTick() {
    if (!manualMode_ || symbol().empty()) return;
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

bool LabItem::settled() const {
    return !loading_ && source_.gpu && stats_->drawnSourceId.load() == source_.gpu->id &&
           !stats_->uploadPending.load();
}

void LabItem::accept(LabSource source, bool preserveView, bool final) {
    source_ = std::move(source);
    loading_ = !final;
    const auto &gpu = *source_.gpu;
    if (!preserveView || !(view_.priceHi > view_.priceLo)) {
        // The whole requested range: while older history loads it draws as "loading".
        const double requested = realMode_ ? double(realHours_) * heatmap::kHourMs : double(source_.endMs - source_.startMs);
        const double span = std::min(requested, heatmap::maxTimeSpanMs(std::max(1.0, width()) * devicePixelRatio(), tfMs()));
        view_.timeHiMs = double(source_.endMs);
        view_.timeLoMs = view_.timeHiMs - span;
        double lo = gpu.coveredPriceLo, hi = gpu.coveredPriceHi;
        if (!(hi > lo)) { lo = 0; hi = 1; }
        const double center = source_.medianPrice > 0 ? source_.medianPrice : (lo + hi) / 2;
        const double half = std::min((hi - lo) / 2, center * 0.02);
        view_.priceLo = center - half;
        view_.priceHi = center + half;
    }
    // Manual presets: every ladder tick some loaded column can build (a finer one
    // than older history supports veils there, with the indicator; spec rule 2).
    std::vector<int64_t> commons;
    for (const double t : heatmap::gpu::columnCommonTicks(gpu)) commons.push_back(heatmap::toUnits(t, gpu.priceScale));
    const int64_t finest = commons.empty() ? 0 : *std::min_element(commons.begin(), commons.end());
    QVariantList offered;
    for (const int64_t units : heatmap::manualPresetUnits(commons, finest * 1000))
        offered.push_back(heatmap::fromUnits(units, gpu.priceScale));
    if (offered != offeredTicks_) { offeredTicks_ = offered; emit presetsChanged(); }
    restoreTick();
    clampView();
    if (final && initialRowPx_ > 0) {
        zoomToRowPx(initialRowPx_);
        initialRowPx_ = 0;
    }
    status_ = QStringLiteral("%1: %2 entries at %3m%4")
                  .arg(realMode_ ? QStringLiteral("Real ") + realLayer_ : QStringLiteral("Synthetic"))
                  .arg(gpu.entryCount).arg(gpu.tfMs / 60'000)
                  .arg(final ? QString() : QStringLiteral(" (loading older history...)"));
    emit statusChanged();
    update();
}

void LabItem::loadReal(int hours, const QString &layer) {
    if (hours < 1 || hours > 24 * 30 || (layer != "near" && layer != "deep")) return;
    realHours_ = hours; realLayer_ = layer; realMode_ = true;
    reload(false);
}
void LabItem::loadSynthetic(int count) {
    if (count < 1 || count > 30'000'000) return;
    realMode_ = false;
    syntheticCount_ = count;
    reload(false);
}

void LabItem::reload(bool preserveView) {
    const auto generation = loadGeneration_->fetch_add(1) + 1;
    loading_ = true;
    status_ = realMode_ ? QStringLiteral("Loading %1 h %2 at %3m...").arg(realHours_).arg(realLayer_).arg(timeframeMinutes_)
                        : QStringLiteral("Generating %1 entries at %2m...").arg(syntheticCount_).arg(timeframeMinutes_);
    emit statusChanged();
    const QPointer<LabItem> self(this);
    const auto guard = loadGeneration_;
    const auto mutex = loadMutex_;
    const bool real = realMode_;
    const int hours = realHours_, count = syntheticCount_;
    const std::string layer = realLayer_.toStdString();
    const int64_t tf = tfMs();
    auto post = [self, guard, generation](std::optional<LabSource> source, QString error, bool preserve, bool final) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
            if (!self || guard->load() != generation) return;
            if (!source) {
                self->loading_ = false;
                self->status_ = unavailableStatus(error);
                emit self->statusChanged();
                return;
            }
            self->accept(*source, preserve, final);
        }, Qt::QueuedConnection);
    };
    QThreadPool::globalInstance()->start([=] {
        std::scoped_lock lock(*mutex);
        if (guard->load() != generation) return;
        try {
            if (!real) {
                post(syntheticSource(uint64_t(count), tf), {}, preserveView, true);
                return;
            }
            // Paint the recent two hours first; older history draws as "loading".
            const int first = std::min(hours, 2);
            post(loadRealSource(layer, hours, first, tf), {}, preserveView, first == hours);
            if (first == hours || guard->load() != generation) return;
            post(loadRealSource(layer, hours, hours, tf), {}, true, true);
        } catch (const std::exception &e) {
            post(std::nullopt, QString::fromUtf8(e.what()), preserveView, true);
        }
    });
}

void LabItem::pan(double dx, double dy) {
    if (!source_.gpu || width() <= 0 || height() <= 0) return;
    const double dt = dx / width() * (view_.timeHiMs - view_.timeLoMs);
    const double dp = dy / height() * (view_.priceHi - view_.priceLo);
    view_.timeLoMs -= dt; view_.timeHiMs -= dt;
    view_.priceLo += dp; view_.priceHi += dp;
    update();
}

void LabItem::setTimeframeMinutes(int minutes) {
    if (minutes < 1 || minutes > 1440 || timeframeMinutes_ == minutes) return;
    timeframeMinutes_ = minutes;
    restoreTick(); // Manual: this timeframe's remembered tick
    clampView();
    emit timeframeChanged();
    // The old source keeps drawing at its own timeframe until the new one is uploaded.
    if (source_.gpu) reload(true);
    update();
}

void LabItem::setManualMode(bool manual) {
    if (manualMode_ == manual) return;
    manualMode_ = manual;
    if (manual && !symbol().empty()) {
        // Lock the remembered tick for (symbol, timeframe), else the tick drawn now.
        // (Before a source loads, restoreTick() resolves it on accept.)
        const auto remembered = tickMemory_.get(symbol(), tfMs());
        manualTick_ = remembered ? heatmap::fromUnits(*remembered, priceScale()) : fallbackManualTick();
        rememberTick();
    }
    // Back to Auto: the node evaluates the Auto rule fresh from the current zoom.
    clampView();
    emit tickChanged();
    update();
}

// The tick drawn now if it is a preset, else the finest preset the loaded data builds.
double LabItem::fallbackManualTick() const {
    const double drawn = stats_->tick.load();
    if (heatmap::isPresetUnits(heatmap::toUnits(drawn, priceScale()))) return drawn;
    if (!offeredTicks_.isEmpty()) return offeredTicks_.front().toDouble();
    return source_.gpu ? heatmap::fromUnits(heatmap::presetAtLeast(0, heatmap::toUnits(heatmap::gpu::commonTick(*source_.gpu),
                                                                                     priceScale())), priceScale())
                       : 1.0;
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
    if (symbol().empty()) explicitTickPending_ = true;
    else rememberTick();
    clampView();
    if (changed) emit tickChanged();
    update();
}

void LabItem::setHysteresis(double h) {
    if (!std::isfinite(h) || h < 0 || h > 0.9 || h == hysteresis_) return;
    hysteresis_ = h;
    emit tickChanged();
    update();
}
void LabItem::setMinRowPx(double px) {
    if (!std::isfinite(px) || px < 0.5 || px > 32 || px == minRowPx_) return;
    minRowPx_ = px;
    emit tickChanged();
    update();
}
void LabItem::setCrossfade(bool enabled) {
    if (crossfade_ == enabled) return;
    crossfade_ = enabled;
    emit tickChanged();
    update();
}

void LabItem::zoom(double steps, bool priceOnly, double anchorX, double anchorY) {
    if (!source_.gpu) return;
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
    const double finest = heatmap::gpu::commonTick(*source_.gpu);
    double desired = std::max(finest * 4, priceSpan * factor);
    if (manualMode_) {
        const double maximum = heatmap::maxManualPriceSpan(std::max(1.0, height()) * dpr, manualTick_);
        if (maximum > 0) desired = std::min(desired, maximum);
    }
    const double a = 1.0 - std::clamp(anchorY, 0.0, 1.0);
    const double center = view_.priceLo + a * priceSpan;
    view_.priceLo = center - a * desired;
    view_.priceHi = view_.priceLo + desired;
    update();
}

void LabItem::wheelZoom(double angleX, double angleY, bool priceOnly, double anchorX, double anchorY) {
    double delta = angleY;
    if (priceOnly && delta == 0) delta = angleX; // macOS: Shift turns the vertical wheel horizontal
    if (delta == 0 || !std::isfinite(delta)) return;
    zoom(delta / 120.0, priceOnly, anchorX, anchorY);
}

void LabItem::zoomToRowPx(double px) {
    if (!source_.gpu || !(px > 0) || !std::isfinite(px) || !(height() > 0)) return;
    const double common = heatmap::gpu::commonTickInView(*source_.gpu, view_.timeLoMs, view_.timeHiMs);
    const double span = height() * devicePixelRatio() * common / px;
    const double center = (view_.priceLo + view_.priceHi) * 0.5;
    view_.priceLo = center - span * 0.5;
    view_.priceHi = center + span * 0.5;
    clampView();
    update();
}

QString LabItem::resolutionIndicator() const {
    if (!manualMode_ || !source_.gpu) return {};
    const auto &gpu = *source_.gpu;
    const double tf = double(gpu.tfMs);
    const auto first = int64_t(std::floor(view_.timeLoMs / tf)), end = int64_t(std::ceil(view_.timeHiMs / tf));
    const auto coverage = heatmap::gpu::tickCoverage(gpu, first, end, manualTick_);
    if (!coverage.incompatible) return {};
    const QString grid = coverage.incompatibleCommon > 0 ? money(coverage.incompatibleCommon) : QStringLiteral("another");
    if (coverage.incompatible == coverage.columns)
        return QStringLiteral("Resolution: %1 unavailable for all data in view (recorded on a %2 grid). Veiled, not coarsened.")
            .arg(money(manualTick_), grid);
    return QStringLiteral("Resolution: %1 unavailable for %2 of %3 columns in view (%4 to %5 UTC, recorded on a %6 grid). Veiled, not coarsened.")
        .arg(money(manualTick_)).arg(coverage.incompatible).arg(coverage.columns)
        .arg(utc(coverage.firstIncompatibleBucket * gpu.tfMs), utc(coverage.endIncompatibleBucket * gpu.tfMs), grid);
}

bool LabItem::saveScreenshot(const QString &path) {
    if (!window() || !source_.gpu) return false;
    const QString target = path.isEmpty() ?
        QStringLiteral("screenshots/lab-%1.png").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")) : path;
    const QFileInfo file(target);
    if (insideRecordingRoot(target)) return false;
    QDir().mkpath(file.absolutePath());
    return window()->grabWindow().save(target, "PNG");
}

QVariantMap LabItem::metrics() const {
    const auto *gpu = source_.gpu.get();
    return {{"fps", frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0}, {"frameMs", frameMs_},
            {"binSubmitMs", stats_->binSubmitMs.load()}, {"gpuFrameMs", stats_->gpuFrameMs.load()},
            {"firstFrameMs", firstFrameMs_}, {"entries", gpu ? qlonglong(gpu->entryCount) : 0},
            {"gpuBytes", qulonglong(stats_->gpuBytes.load())},
            {"columns", stats_->columns.load()}, {"rows", stats_->rows.load()},
            {"group", stats_->factor.load()}, {"tick", stats_->tick.load()},
            {"rebins", qulonglong(stats_->rebins.load())}, {"timeframeMinutes", timeframeMinutes_},
            {"mode", manualMode_ ? QStringLiteral("manual") : QStringLiteral("auto")},
            {"manualTick", manualTick_}, {"hysteresis", hysteresis_}, {"minRowPx", minRowPx_},
            {"crossfadeMs", crossfade_ ? kCrossfadeMs : 0.0}, {"crossfading", stats_->crossfading.load()},
            {"commonTick", stats_->commonTick.load()}, {"tickChanges", qulonglong(stats_->tickChanges.load())},
            {"tickChangeBinMs", stats_->tickChangeBinMs.load()},
            {"rowPx", stats_->tick.load() > 0 && view_.priceHi > view_.priceLo ?
                          stats_->tick.load() * height() * devicePixelRatio() / (view_.priceHi - view_.priceLo) : 0.0},
            {"priceSpan", view_.priceHi - view_.priceLo}, {"timeSpanMin", (view_.timeHiMs - view_.timeLoMs) / 60'000.0},
            {"indicator", resolutionIndicator()},
            {"uploadPending", stats_->uploadPending.load()}, {"errors", qulonglong(stats_->errors.load())},
            {"loadMs", source_.loadMs}, {"composeMs", source_.composeMs}, {"buildMs", source_.buildMs},
            {"settled", settled()},
            {"kernel", stats_->preciseKernel.load() ? QStringLiteral("precise") : QStringLiteral("fast")}};
}
} // namespace lab
