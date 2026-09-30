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
QString prepModeName(PrepMode mode) {
    switch (mode) {
    case PrepMode::Full: return QStringLiteral("full");
    case PrepMode::Viewport: return QStringLiteral("viewport");
    case PrepMode::WholeChunkGpu: return QStringLiteral("whole-chunk");
    case PrepMode::WholeChunkCpu: return QStringLiteral("whole-chunk-cpu");
    case PrepMode::Hybrid: return QStringLiteral("hybrid");
    }
    return {};
}
std::optional<PrepMode> parsePrepMode(const QString &name) {
    for (const auto mode : {PrepMode::Full, PrepMode::Viewport, PrepMode::WholeChunkGpu, PrepMode::WholeChunkCpu,
                            PrepMode::Hybrid})
        if (prepModeName(mode) == name) return mode;
    return std::nullopt;
}

namespace {
std::atomic<uint64_t> nextTileId{1};
double msSince(const QElapsedTimer &timer) { return timer.nsecsElapsed() / 1e6; }
QString chunkKeyText(const heatmap::ChunkKey &key) {
    return QStringLiteral("%1/%2").arg(key.levelMs).arg(key.startMs);
}
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
                if (firstFrameMs_ == 0 && (stats_->drawnSourceId.load() != 0 || tileStats_->drawnPrimary.load() != 0))
                    firstFrameMs_ = launched_.nsecsElapsed() / 1e6;
                update();
            }, Qt::QueuedConnection);
        }
    }
    QQuickItem::itemChange(change, value);
}

QSGNode *LabItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    // A plain root whose one child is the node of the current prep mode.
    QSGNode *root = old ? old : new QSGNode;
    const int want = wholeChunk() ? 1 : 0;
    if (shownNode_ != want || !root->firstChild()) {
        if (QSGNode *child = root->firstChild()) {
            root->removeChildNode(child);
            delete child; // render thread; the node releases its GPU resources
        }
        if (want) root->appendChildNode(new heatmap::gpu::HeatmapTileNode(tileStats_));
        else root->appendChildNode(new heatmap::gpu::HeatmapRenderNode(stats_));
        shownNode_ = want;
    }
    if (want == 0) {
        auto *node = static_cast<heatmap::gpu::HeatmapRenderNode *>(root->firstChild());
        heatmap::gpu::HeatmapRenderNode::Frame frame;
        frame.source = chunked() ? vSource_.gpu : source_.gpu;
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
        frame.uploadBudgetBytes = uploadBudget_;
        node->setFrame(std::move(frame));
        return root;
    }
    auto *node = static_cast<heatmap::gpu::HeatmapTileNode *>(root->firstChild());
    // Resident tiles no longer need their CPU copy (GUI thread is blocked here).
    const auto residentIds = tileStats_->residentIds();
    const std::set<uint64_t> resident(residentIds.begin(), residentIds.end());
    size_t cpuBytes = 0;
    wTiles_.forEachMutable([&](const heatmap::tiles::TileKey &, WTile &tile, size_t) {
        if (resident.count(tile.ref.id)) {
            tile.ref.cells.reset();
            if (!tile.ref.viewRows) tile.ref.source.reset(); // hybrid tiles re-bin from their resident source
        }
        if (tile.ref.cells) cpuBytes += tile.ref.cells->size() * 4;
    });
    wCpuBytes_ = cpuBytes;
    heatmap::gpu::HeatmapTileNode::Frame frame;
    const int64_t tf = tfMs();
    wTiles_.forEach([&](const heatmap::tiles::TileKey &, const WTile &tile, size_t) { frame.tiles.push_back(tile.ref); });
    const auto visible = heatmap::tiles::tilesCovering(view_.timeLoMs, view_.timeHiMs, tf, 0);
    for (int64_t t = visible.first; t < visible.end && wTickUnits_ > 0; ++t) {
        heatmap::gpu::TileSlot slot;
        slot.firstBucket = heatmap::tiles::tileFirstBucket(t);
        slot.endBucket = heatmap::tiles::tileFirstBucket(t + 1);
        slot.expected = heatmap::tiles::tileEndMs(t, tf) > availability_.oldestMs &&
                        heatmap::tiles::tileStartMs(t, tf) < availability_.endMs;
        const TileBase base{tf, wTickUnits_, t};
        if (const auto it = wLatest_.find(base); it != wLatest_.end())
            if (const auto *tile = wTiles_.peek(it->second)) (tileStale(*tile) ? slot.fallback : slot.primary) = tile->ref.id;
        if (!slot.fallback)
            if (const auto it = wPrevious_.find(base); it != wPrevious_.end())
                if (const auto *tile = wTiles_.peek(it->second)) slot.fallback = tile->ref.id;
        frame.visible.push_back(slot);
    }
    frame.key = targetKey();
    frame.tfMs = tf;
    frame.view = view_;
    frame.rect = QRectF(0, 0, width(), height());
    frame.uploadBudgetBytes = uploadBudget_;
    frame.crossfadeMs = crossfade_ ? kCrossfadeMs : 0;
    node->setFrame(std::move(frame));
    return root;
}

void LabItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickItem::geometryChange(next, previous);
    clampView();
    update();
}

double LabItem::devicePixelRatio() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

std::string LabItem::symbol() const {
    if (chunked()) return kSymbol;
    return source_.gpu ? source_.gpu->symbol : std::string();
}
double LabItem::priceScale() const {
    if (chunked()) return vSource_.gpu ? vSource_.gpu->priceScale : 100.0; // BTC grids use 100 units per dollar
    return source_.gpu ? source_.gpu->priceScale : 100.0;
}
bool LabItem::hasContent() const {
    if (!chunked()) return source_.gpu != nullptr;
    return haveAvailability_;
}
uint64_t LabItem::targetKey() const {
    return uint64_t(tfMs()) * 1'000'003ull ^ uint64_t(wTickUnits_) * 0x9e3779b97f4a7c15ull;
}
double LabItem::finestTick() const {
    if (!chunked()) return source_.gpu ? heatmap::gpu::commonTick(*source_.gpu) : 1.0;
    if (!wholeChunk() && vSource_.gpu) return heatmap::gpu::commonTick(*vSource_.gpu);
    const int64_t units = cachedCommonUnits(layer(), tfMs(), view_.timeLoMs, view_.timeHiMs);
    return units > 0 ? heatmap::fromUnits(units, priceScale()) : 1.0;
}
double LabItem::commonTickInViewAny() const {
    const heatmap::gpu::GpuSource *gpu = !chunked() ? source_.gpu.get() : wholeChunk() ? nullptr : vSource_.gpu.get();
    if (gpu) return heatmap::gpu::commonTickInView(*gpu, view_.timeLoMs, view_.timeHiMs);
    const int64_t units = chunked() ? cachedCommonUnits(layer(), tfMs(), view_.timeLoMs, view_.timeHiMs) : 0;
    return units > 0 ? heatmap::fromUnits(units, priceScale()) : 0;
}

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
    if (chunked()) {
        if (!haveAvailability_) return false;
        if (!wholeChunk())
            return !vInFlight_ && vSource_.gpu && viewportCovers() && stats_->drawnSourceId.load() == vSource_.gpu->id &&
                   !stats_->uploadPending.load();
        if (wTickUnits_ <= 0 || !tileStats_->complete.load() || tileStats_->drawnKey.load() != targetKey()) return false;
        const auto visible = heatmap::tiles::tilesCovering(view_.timeLoMs, view_.timeHiMs, tfMs(), 0);
        for (int64_t t = visible.first; t < visible.end; ++t)
            if (wInFlight_.count({tfMs(), wTickUnits_, t})) return false;
        return true;
    }
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
    if (prepMode_ != PrepMode::Full) startChunked(false);
    else reload(false);
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
    if (!hasContent() || width() <= 0 || height() <= 0) return;
    const double dt = dx / width() * (view_.timeHiMs - view_.timeLoMs);
    const double dp = dy / height() * (view_.priceHi - view_.priceLo);
    view_.timeLoMs -= dt; view_.timeHiMs -= dt;
    view_.priceLo += dp; view_.priceHi += dp;
    viewChanged();
}

void LabItem::setTimeframeMinutes(int minutes) {
    if (minutes < 1 || minutes > 1440 || timeframeMinutes_ == minutes) return;
    timeframeMinutes_ = minutes;
    restoreTick(); // Manual: this timeframe's remembered tick
    clampView();
    emit timeframeChanged();
    // The old source keeps drawing at its own timeframe until the new one is uploaded.
    if (chunked()) viewChanged();
    else if (source_.gpu) reload(true);
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
    wAutoUnits_ = 0;
    clampView();
    emit tickChanged();
    viewChanged();
}

// The tick drawn now if it is a preset, else the finest preset the loaded data builds.
double LabItem::fallbackManualTick() const {
    const double drawn = stats_->tick.load();
    if (heatmap::isPresetUnits(heatmap::toUnits(drawn, priceScale()))) return drawn;
    if (wholeChunk() && wTickUnits_ > 0) return heatmap::fromUnits(wTickUnits_, priceScale());
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
    const double finest = finestTick();
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
    const double common = commonTickInViewAny();
    if (!(common > 0)) return;
    const double span = height() * devicePixelRatio() * common / px;
    const double center = (view_.priceLo + view_.priceHi) * 0.5;
    view_.priceLo = center - span * 0.5;
    view_.priceHi = center + span * 0.5;
    clampView();
    viewChanged();
}

QString LabItem::resolutionIndicator() const {
    if (!manualMode_) return {};
    if (wholeChunk()) {
        const int64_t tick = heatmap::toUnits(manualTick_, priceScale());
        int64_t bad = 0;
        for (const int64_t units : cachedColumnCommonUnits(layer(), tfMs(), view_.timeLoMs, view_.timeHiMs))
            if (!heatmap::buildsOn(tick, units)) bad = bad ? std::lcm(bad, units) : units;
        if (!bad) return {};
        return QStringLiteral("Resolution: %1 unavailable for some columns in view (recorded on a %2 grid). Veiled, not coarsened.")
            .arg(money(manualTick_), money(heatmap::fromUnits(bad, priceScale())));
    }
    const heatmap::gpu::GpuSource *source = chunked() ? vSource_.gpu.get() : source_.gpu.get();
    if (!source) return {};
    const auto &gpu = *source;
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
    if (!window() || !hasContent()) return false;
    const QString target = path.isEmpty() ?
        QStringLiteral("screenshots/lab-%1.png").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")) : path;
    const QFileInfo file(target);
    const QString protectedRoot = QString::fromLatin1(kRecordingRoot);
    const QString canonicalParent = file.absoluteDir().canonicalPath();
    if (file.absoluteFilePath().startsWith(protectedRoot) || canonicalParent.startsWith(protectedRoot)) return false;
    QDir().mkpath(file.absolutePath());
    return window()->grabWindow().save(target, "PNG");
}

QVariantMap LabItem::metrics() const {
    const auto *gpu = chunked() ? vSource_.gpu.get() : source_.gpu.get();
    QVariantMap m{{"fps", frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0}, {"frameMs", frameMs_},
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
    if (wholeChunk()) {
        const double tick = heatmap::fromUnits(wTickUnits_, priceScale());
        m["tick"] = tick;
        m["commonTick"] = commonTickInViewAny();
        m["rowPx"] = tick > 0 && view_.priceHi > view_.priceLo ? tick * height() * devicePixelRatio() / (view_.priceHi - view_.priceLo) : 0.0;
        m["gpuBytes"] = qulonglong(tileStats_->gpuBytes.load());
        m["gpuFrameMs"] = tileStats_->gpuFrameMs.load();
        m["crossfading"] = tileStats_->crossfading.load();
        m["errors"] = qulonglong(tileStats_->errors.load());
        m["uploadPending"] = !tileStats_->complete.load();
        m["columns"] = qulonglong(tileStats_->slotCount.load() * heatmap::tiles::kTileColumns);
    }
    m.insert(prepMetrics());
    return m;
}

// ------------------------------------------------------------------ B1 prep modes

void LabItem::setPrepModeString(const QString &mode) {
    if (const auto parsed = parsePrepMode(mode)) setPrepMode(*parsed);
}

void LabItem::setPrepMode(PrepMode mode) {
    if (mode == prepMode_) return;
    const bool wasChunked = chunked();
    prepMode_ = mode;
    emit prepModeChanged();
    if (!realMode_) { update(); return; } // synthetic data: always the full path
    if (mode == PrepMode::Full) {
        resetChunkedState();
        reload(true);
    } else {
        source_ = LabSource{}; // the full-range source is not used by V/W
        loading_ = false;
        if (!wasChunked || !haveAvailability_) startChunked(true);
        else viewChanged();
    }
    update();
}

void LabItem::setTileBudgetBytes(uint64_t bytes) {
    wTiles_.setMaxBytes(bytes);
    viewChanged();
}

void LabItem::setView(const heatmap::gpu::ViewWindow &view) {
    view_ = view;
    viewChanged();
}

void LabItem::viewChanged() {
    if (chunked()) {
        if (wholeChunk()) updateWholeChunk();
        else updateViewport();
    }
    update();
}

void LabItem::resetChunkedState() {
    ++chunkSerial_; // results of builds in flight are dropped
    vSource_ = ViewportSource{};
    vInFlight_ = false;
    wTiles_.clear();
    wLatest_.clear();
    wPrevious_.clear();
    wInFlight_.clear();
    chunkLoadsInFlight_.clear();
    wAutoUnits_ = wTickUnits_ = 0;
}

void LabItem::startChunked(bool preserveView) {
    resetChunkedState();
    haveAvailability_ = false;
    const uint64_t serial = chunkSerial_;
    status_ = QStringLiteral("Loading %1 (%2)...").arg(realLayer_, prepModeName(prepMode_));
    emit statusChanged();
    const QPointer<LabItem> self(this);
    const std::string layerName = layer();
    QThreadPool::globalInstance()->start([=] {
        LayerInfo info;
        double price = 0;
        try {
            info = layerInfo(layerName, true);
            if (info.error.empty()) price = recentMedianPrice(layerName);
        } catch (const std::exception &e) {
            info.error = e.what();
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
            if (!self || self->chunkSerial_ != serial) return;
            if (!info.error.empty()) {
                self->status_ = unavailableStatus(QString::fromStdString(info.error));
                emit self->statusChanged();
                return;
            }
            self->availability_ = info.availability;
            self->haveAvailability_ = true;
            if (self->pendingView_) {
                self->view_ = *self->pendingView_;
                self->pendingView_.reset();
            } else if (!preserveView || !(self->view_.priceHi > self->view_.priceLo)) {
                const int64_t tf = self->tfMs();
                const double end = double(recording::floorDiv(info.availability.endMs + tf - 1, tf) * tf);
                const double span = std::min(double(self->realHours_) * heatmap::kHourMs,
                                             heatmap::maxTimeSpanMs(std::max(1.0, self->width()) * self->devicePixelRatio(), tf));
                self->view_.timeHiMs = end;
                self->view_.timeLoMs = end - span;
                const double center = price > 0 ? price : 100'000;
                self->view_.priceLo = center * 0.98;
                self->view_.priceHi = center * 1.02;
            }
            self->status_ = QStringLiteral("Real %1 · prep %2").arg(self->realLayer_, prepModeName(self->prepMode_));
            emit self->statusChanged();
            self->restoreTick();
            self->refreshChunkPresets();
            self->clampView();
            if (self->initialRowPx_ > 0 && self->commonTickInViewAny() > 0) {
                self->zoomToRowPx(self->initialRowPx_); // the newest chunks are decoded by now
                self->initialRowPx_ = 0;
            }
            self->viewChanged();
        }, Qt::QueuedConnection);
    });
}

bool LabItem::viewportCovers() const {
    if (!vSource_.gpu || vSource_.gpu->tfMs != tfMs()) return false;
    for (const auto &[key, generation] : vSource_.chunkGenerations) {
        const uint64_t now = chunkStore().generationOf(key);
        if (now && now != generation) return false; // a revised chunk intersects the source
    }
    const auto &a = availability_;
    const double lo = std::max(view_.timeLoMs, double(a.oldestMs)), hi = std::min(view_.timeHiMs, double(a.endMs));
    const auto &r = vSource_.region;
    const bool time = !(hi > lo) || (r.timeLoMs <= lo && r.timeHiMs >= hi);
    return time && r.priceLo <= view_.priceLo && r.priceHi >= view_.priceHi;
}

void LabItem::updateViewport() {
    if (!chunked() || wholeChunk() || !haveAvailability_ || !(width() > 0) || !(height() > 0)) return;
    if (vInFlight_ || viewportCovers()) return; // a build in flight re-checks when it lands
    // Prepared region: the view plus one view width/height on each side (plan prefetch rule).
    const double w = view_.timeHiMs - view_.timeLoMs, h = view_.priceHi - view_.priceLo;
    const heatmap::gpu::ViewWindow region{view_.timeLoMs - w, view_.timeHiMs + w, view_.priceLo - h, view_.priceHi + h};
    vInFlight_ = true;
    const uint64_t serial = chunkSerial_;
    const QPointer<LabItem> self(this);
    const std::string layerName = layer();
    const int64_t tf = tfMs();
    QElapsedTimer requested;
    requested.start();
    QThreadPool::globalInstance()->start([=] {
        std::optional<ViewportSource> built;
        QString error;
        try {
            built = buildViewportSource(layerName, tf, region);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
            if (!self || self->chunkSerial_ != serial) return;
            self->vInFlight_ = false;
            if (!built) {
                self->status_ = unavailableStatus(error);
                emit self->statusChanged();
                return;
            }
            const bool loaded = built->timing.chunkLoadsAfter > built->timing.chunkLoadsBefore;
            self->vSource_ = std::move(*built);
            ++self->vBuilds_;
            self->vUploadBytes_ += self->vSource_.gpu ? self->vSource_.gpu->bytes() : 0;
            self->vLastRequestMs_ = msSince(requested);
            if (loaded) self->refreshChunkPresets();
            self->viewChanged(); // the view may have moved on while this built
        }, Qt::QueuedConnection);
    });
}

bool LabItem::tileStale(const WTile &tile) const {
    for (const auto &[key, generation] : tile.chunkGenerations) {
        const uint64_t now = chunkStore().generationOf(key);
        if (now && now != generation) return true;
    }
    return false;
}

void LabItem::requestChunks(double loMs, double hiMs) {
    if (!(hiMs > loMs)) return;
    const int64_t tf = tfMs();
    const auto keys = chunkKeysFor(layer(), tf, int64_t(std::floor(loMs / double(tf))) * tf,
                                   int64_t(std::ceil(hiMs / double(tf))) * tf);
    const uint64_t serial = chunkSerial_;
    const QPointer<LabItem> self(this);
    for (const auto &key : keys) {
        const std::string id = chunkKeyText(key).toStdString();
        if (chunkStore().contains(key) || chunkLoadsInFlight_.count(id)) continue;
        chunkLoadsInFlight_.insert(id);
        QThreadPool::globalInstance()->start([=] {
            try { chunkStore().get(key); } catch (const std::exception &) {}
            QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
                if (!self || self->chunkSerial_ != serial) return;
                self->chunkLoadsInFlight_.erase(id);
                self->refreshChunkPresets();
                self->viewChanged();
            }, Qt::QueuedConnection);
        });
    }
}

void LabItem::refreshChunkPresets() {
    if (!chunked() || !haveAvailability_) return;
    const auto commons = cachedColumnCommonUnits(layer(), tfMs(), double(availability_.oldestMs), double(availability_.endMs));
    if (commons.empty()) return;
    const int64_t finest = *std::min_element(commons.begin(), commons.end());
    QVariantList offered;
    for (const int64_t units : heatmap::manualPresetUnits(commons, finest * 1000))
        offered.push_back(heatmap::fromUnits(units, priceScale()));
    if (offered != offeredTicks_) { offeredTicks_ = offered; emit presetsChanged(); }
}

void LabItem::updateWholeChunk() {
    if (!wholeChunk() || !haveAvailability_ || !(width() > 0) || !(height() > 0)) return;
    const int64_t tf = tfMs();
    const auto &a = availability_;
    const double dataLo = std::max(view_.timeLoMs, double(a.oldestMs)), dataHi = std::min(view_.timeHiMs, double(a.endMs));
    // Tick: Manual obeys; Auto follows the data in view (chunks must be decoded
    // to know its native grids; until then the previous tick keeps drawing).
    int64_t tickUnits = 0;
    if (manualMode_) {
        tickUnits = heatmap::toUnits(manualTick_, priceScale());
        if (!heatmap::isPresetUnits(tickUnits)) tickUnits = 0;
    } else {
        const int64_t common = dataHi > dataLo ? cachedCommonUnits(layer(), tf, dataLo, dataHi) : 0;
        const auto keys = dataHi > dataLo ? chunkKeysFor(layer(), tf, int64_t(std::floor(dataLo / double(tf))) * tf,
                                                         int64_t(std::ceil(dataHi / double(tf))) * tf)
                                          : std::vector<heatmap::ChunkKey>{};
        bool allCached = true;
        for (const auto &key : keys) allCached = allCached && chunkStore().contains(key);
        if (!allCached) requestChunks(dataLo, dataHi);
        if (common > 0) {
            const double unitsPerPx = (view_.priceHi - view_.priceLo) * priceScale() / std::max(1.0, height() * devicePixelRatio());
            tickUnits = heatmap::autoTickUnits(wAutoUnits_, common, unitsPerPx, {minRowPx_, hysteresis_});
            if (tickUnits > 0) wAutoUnits_ = tickUnits;
        }
    }
    if (tickUnits <= 0) tickUnits = wTickUnits_;
    if (tickUnits <= 0) { update(); return; }
    if (tickUnits != wTickUnits_) emit tickChanged();
    wTickUnits_ = tickUnits;
    const double center = (view_.priceLo + view_.priceHi) * 0.5;
    const int64_t centerBin = recording::floorDiv(heatmap::toUnits(center, priceScale()), tickUnits);
    const auto visible = heatmap::tiles::tilesCovering(view_.timeLoMs, view_.timeHiMs, tf, 0);
    const auto wanted = heatmap::tiles::tilesCovering(view_.timeLoMs, view_.timeHiMs, tf, kPrefetchTiles);
    std::vector<int64_t> order;
    for (int64_t t = visible.first; t < visible.end; ++t) order.push_back(t);
    for (int64_t t = wanted.first; t < visible.first; ++t) order.push_back(t);
    for (int64_t t = visible.end; t < wanted.end; ++t) order.push_back(t);
    const uint64_t serial = chunkSerial_;
    const QPointer<LabItem> self(this);
    const std::string layerName = layer();
    const auto builder = prepMode_ == PrepMode::WholeChunkCpu ? TileBuilder::Cpu : TileBuilder::Gpu;
    for (const int64_t t : order) {
        if (heatmap::tiles::tileEndMs(t, tf) <= a.oldestMs || heatmap::tiles::tileStartMs(t, tf) >= a.endMs) continue;
        const TileBase base{tf, tickUnits, t};
        if (wInFlight_.count(base)) continue;
        if (const auto it = wLatest_.find(base); it != wLatest_.end())
            if (const auto *tile = wTiles_.peek(it->second); tile && !tileStale(*tile)) {
                wTiles_.find(it->second); // touch: in or next to the view
                continue;
            }
        if (builder == TileBuilder::Gpu) {
            // Spans whose chunks and GpuSource are cached need no worker round trip.
            if (const auto cached = cachedGpuTile(layerName, tf, tickUnits, t, centerBin)) {
                acceptTile(base, *cached);
                continue;
            }
        }
        wInFlight_.insert(base);
        QThreadPool::globalInstance()->start([=] {
            std::optional<TileBuild> built;
            QString error;
            try {
                built = buildTile(layerName, tf, tickUnits, t, builder, centerBin);
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
                if (!self || self->chunkSerial_ != serial) return;
                self->wInFlight_.erase(base);
                if (!built || built->empty) {
                    if (!built) {
                        self->status_ = unavailableStatus(error);
                        emit self->statusChanged();
                    }
                    return;
                }
                self->acceptTile(base, *built);
                self->viewChanged();
            }, Qt::QueuedConnection);
        });
    }
    // Budget: never evict what is in view at the target tick or still drawn
    // (a held picture during a transition, a fading set, a fallback).
    const auto drawnIds = tileStats_->drawnIds();
    const std::set<uint64_t> drawn(drawnIds.begin(), drawnIds.end());
    const auto evicted = wTiles_.evict([&](const heatmap::tiles::TileKey &key) {
        if (key.tfMs == tf && key.tickUnits == tickUnits && visible.contains(key.tile)) return true;
        const auto *tile = wTiles_.peek(key);
        return tile && drawn.count(tile->ref.id);
    });
    for (const auto &key : evicted) {
        const TileBase base{key.tfMs, key.tickUnits, key.tile};
        if (const auto it = wLatest_.find(base); it != wLatest_.end() && it->second == key) wLatest_.erase(it);
        if (const auto it = wPrevious_.find(base); it != wPrevious_.end() && it->second == key) wPrevious_.erase(it);
    }
    update();
}

void LabItem::acceptTile(const TileBase &base, const TileBuild &built) {
    WTile tile;
    tile.ref.id = nextTileId.fetch_add(1);
    tile.ref.grid = {built.grid.tfMs, built.grid.firstBucket, built.grid.columns, built.grid.tick,
                     built.grid.firstBin, built.grid.rows};
    tile.ref.cells = built.cells;
    tile.ref.source = built.source;
    tile.ref.viewRows = prepMode_ == PrepMode::Hybrid;
    tile.chunkGenerations = built.chunkGenerations;
    tile.clipped = built.clipped;
    // Budget: what the tile holds on the GPU (hybrid: its resident source).
    const auto bytes = size_t(tile.ref.viewRows && built.source ? built.source->bytes() : tile.ref.cellBytes());
    if (const auto it = wLatest_.find(base); it != wLatest_.end() && !(it->second == built.key))
        wPrevious_[base] = it->second;
    wLatest_[base] = built.key;
    wTiles_.insert(built.key, std::move(tile), bytes);
    ++wBuilds_;
    wIntermediateHits_ += built.intermediateHit;
    wClipped_ += built.clipped;
    wLastBuildMs_ = built.timing.totalMs;
    if (built.timing.chunkLoadsAfter > built.timing.chunkLoadsBefore) refreshChunkPresets();
}

void LabItem::reviseNewestChunk() {
    if (!chunked() || !haveAvailability_) return;
    const uint64_t serial = chunkSerial_;
    const QPointer<LabItem> self(this);
    const std::string layerName = layer();
    QThreadPool::globalInstance()->start([=] {
        LayerInfo info;
        try {
            info = layerInfo(layerName, true);
            if (info.error.empty()) {
                const int64_t newest = recording::floorDiv(info.availability.endMs - 1, heatmap::kHourMs) * heatmap::kHourMs;
                chunkStore().reload({kSymbol, layerName, heatmap::kMinuteMs, newest});
            }
        } catch (const std::exception &) {
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [=] {
            if (!self || self->chunkSerial_ != serial || !info.error.empty()) return;
            self->availability_ = info.availability;
            self->viewChanged();
        }, Qt::QueuedConnection);
    });
}

QVariantMap LabItem::prepMetrics() const {
    const auto store = chunkStore().stats();
    const auto inter = intermediateStats();
    QVariantMap m{{"prep", prepModeName(prepMode_)},
                  {"chunkBytes", qulonglong(store.bytes)}, {"chunkEntries", qulonglong(store.entries)},
                  {"chunkHits", qulonglong(store.hits)}, {"chunkMisses", qulonglong(store.misses)},
                  {"chunkLoads", qulonglong(store.loads)}, {"chunkSharedLoads", qulonglong(store.sharedLoads)},
                  {"chunkEvictions", qulonglong(store.evictions)}, {"chunkLoadMs", store.loadMs},
                  {"interBytes", qulonglong(inter.bytes)}, {"interHits", qulonglong(inter.hits)},
                  {"interMisses", qulonglong(inter.misses)},
                  {"footprintBytes", qulonglong(processFootprintBytes())}};
    if (!chunked()) {
        m["prepCpuBytes"] = qulonglong(source_.gpu ? source_.gpu->bytes() : 0);
        m["lastPrepMs"] = source_.loadMs + source_.composeMs + source_.buildMs;
        return m;
    }
    if (!wholeChunk()) {
        m["prepCpuBytes"] = qulonglong(vSource_.cpuBytes);
        m["lastPrepMs"] = vSource_.timing.totalMs;
        m["lastPrepChunkMs"] = vSource_.timing.chunkMs;
        m["lastPrepComposeMs"] = vSource_.timing.composeMs;
        m["lastPrepBuildMs"] = vSource_.timing.buildMs;
        m["lastRequestMs"] = vLastRequestMs_;
        m["prepBuilds"] = qulonglong(vBuilds_);
        m["prepUploadBytes"] = qulonglong(vUploadBytes_);
        m["prepSourceEntries"] = qulonglong(vSource_.gpu ? vSource_.gpu->entryCount : 0);
        m["prepInFlight"] = vInFlight_ ? 1 : 0;
        m["prepChunks"] = qulonglong(vSource_.timing.chunks);
        m["regionMinutes"] = (vSource_.region.timeHiMs - vSource_.region.timeLoMs) / 60'000.0;
        m["regionPrice"] = vSource_.region.priceHi - vSource_.region.priceLo;
        return m;
    }
    m["prepCpuBytes"] = qulonglong(wCpuBytes_);
    m["lastPrepMs"] = wLastBuildMs_;
    m["prepBuilds"] = qulonglong(wBuilds_);
    m["prepInFlight"] = qulonglong(wInFlight_.size());
    m["tiles"] = qulonglong(wTiles_.size());
    m["tileBytes"] = qulonglong(wTiles_.bytes());
    m["tileHits"] = qulonglong(wTiles_.hits());
    m["tileMisses"] = qulonglong(wTiles_.misses());
    m["tileEvictions"] = qulonglong(wTiles_.evictions());
    m["tilesResident"] = qulonglong(tileStats_->residentTiles.load());
    m["tilesUploaded"] = qulonglong(tileStats_->tilesUploaded.load());
    m["tilesBinned"] = qulonglong(tileStats_->tilesBinned.load());
    m["tilesRebinned"] = qulonglong(tileStats_->rebinnedTiles.load());
    m["tileUploadBytes"] = qulonglong(tileStats_->uploadBytes.load());
    m["prepUploadBytes"] = qulonglong(tileStats_->uploadBytes.load());
    m["binnerBytes"] = qulonglong(tileStats_->binnerBytes.load());
    m["tileGpuBytes"] = qulonglong(tileStats_->residentBytes.load());
    m["tileNodePrepareMs"] = tileStats_->prepareMs.load();
    m["intermediateHits"] = qulonglong(wIntermediateHits_);
    m["clippedTiles"] = qulonglong(wClipped_);
    m["loadingSlots"] = tileStats_->loadingSlots.load();
    m["holding"] = tileStats_->holding.load();
    return m;
}
} // namespace lab
