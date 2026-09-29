#include "LabItem.hpp"
#include "servermodel/PriceLadder.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QQuickWindow>
#include <QPointer>
#include <QThreadPool>
#include <rhi/qrhi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
const auto launched = Clock::now();
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
uint32_t sourceMinutesFor(int timeframe, const QString &) {
    return uint32_t(std::max(1, timeframe));
}
QString unavailableStatus(QString detail) {
    const QString prefix = QStringLiteral("timeframe unavailable:");
    if (detail.startsWith(prefix, Qt::CaseInsensitive)) detail = detail.mid(prefix.size());
    return QStringLiteral("Timeframe unavailable: %1").arg(detail.trimmed());
}
} // namespace

class LabRenderer final : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer *) override {
        if (!binner_ || resourceRhi_ != rhi()) {
            binner_ = std::make_unique<GpuBinner>(rhi());
            resourceRhi_ = rhi();
            uploaded_.reset();
            pending_.reset();
            pendingData_.reset();
            lastGrid_.reset();
        }
    }
    void synchronize(QQuickRhiItem *item) override {
        auto *lab = static_cast<LabItem *>(item);
        data_ = lab->data_; view_ = lab->view_; version_ = lab->version_;
        timeframeMinutes_ = lab->timeframeMinutes_; manualTick_ = lab->manualTick_;
        realMode_ = lab->realMode_;
        telemetry_ = lab->telemetry_;
    }
    void render(QRhiCommandBuffer *cb) override {
        const auto now = Clock::now();
        if (previous_ != Clock::time_point{}) telemetry_->frameMs.store(msSince(previous_));
        previous_ = now;
        telemetry_->frames.fetch_add(1);
        if (!data_ || !binner_) { update(); return; }
        QString error;
        bool uploadedNow = false;
        if (uploaded_ != data_ && pendingData_ != data_) {
            pending_ = std::make_unique<GpuBinner>(rhi());
            if (!pending_->beginUpload(*data_, &error)) { qWarning("sentinel-lab upload: %s", qPrintable(error)); return; }
            pendingData_ = data_;
        }
        if (pending_) {
            if (!pending_->uploadStep(cb, 2 * 1024 * 1024, &error)) {
                qWarning("sentinel-lab upload: %s", qPrintable(error)); return;
            }
            if (pending_->uploadComplete()) {
                binner_ = std::move(pending_);
                uploaded_ = std::move(pendingData_);
                uploadedNow = true;
                lastGrid_.reset();
            }
        }
        if (!uploaded_) { update(); return; }
        const auto &active = *uploaded_;
        const int tf = std::max(1, timeframeMinutes_);
        // A real timeframe switch must wait for its composed source. Never
        // reinterpret the resident minute buffer as a coarse timeframe.
        if (realMode_ && active.sourceMinutes != uint32_t(tf)) { update(); return; }
        const auto size = renderTarget()->pixelSize();
        const double native = active.nativeTick;
        const double height = std::max(1, size.height());
        const double pricePerPx = (view_.priceHi - view_.priceLo) / height;
        const double tick = manualTick_ > 0 ? manualTick_ :
            recording::ladderTick(pricePerPx * 2.0, native, active.priceScale);
        if (!(tick > 0) || !std::isfinite(tick)) return;
        const auto group = static_cast<uint32_t>(std::llround(tick / native));
        if (!group || std::abs(tick / native - group) > 1e-8) return;
        const double absoluteMinuteLo = double(data_->startMs) / 60'000.0 + view_.timeLo;
        const double absoluteMinuteHi = double(data_->startMs) / 60'000.0 + view_.timeHi;
        const int64_t visibleTimeFirst = int64_t(std::floor(absoluteMinuteLo / tf));
        const int64_t visibleTimeEnd = int64_t(std::ceil(absoluteMinuteHi / tf));
        const int64_t visiblePriceFirst = int64_t(std::floor(view_.priceLo / tick));
        const int64_t visiblePriceEnd = int64_t(std::ceil(view_.priceHi / tick));
        const bool inside = lastGrid_ && !uploadedNow && lastGrid_->group == group &&
            lastGrid_->timeframeMinutes == uint32_t(tf) &&
            gridContainsView(*lastGrid_, active.startMs / 60'000, active.baseRow,
                             absoluteMinuteLo, absoluteMinuteHi, view_.priceLo, view_.priceHi, tick);
        if (!inside) {
            const int64_t firstTime = visibleTimeFirst - 2;
            const int64_t firstPrice = visiblePriceFirst - 2;
            const int64_t firstMinute = firstTime * tf - active.startMs / 60'000;
            const int64_t rowLo = firstPrice * group - active.baseRow;
            const int64_t columns = visibleTimeEnd - firstTime + 2;
            const int64_t rows = visiblePriceEnd - firstPrice + 2;
            if (firstMinute < std::numeric_limits<int32_t>::min() ||
                firstMinute > std::numeric_limits<int32_t>::max() ||
                rowLo < std::numeric_limits<int32_t>::min() ||
                rowLo > std::numeric_limits<int32_t>::max() ||
                active.baseRow < std::numeric_limits<int32_t>::min() ||
                active.baseRow > std::numeric_limits<int32_t>::max() ||
                columns < 1 || columns > 16384 || rows < 1 || rows > 16384) return;
            Grid grid;
            grid.firstMinute = int32_t(firstMinute);
            grid.rowLo = int32_t(rowLo);
            grid.baseRow = int32_t(active.baseRow);
            grid.timeframeMinutes = uint32_t(tf);
            grid.group = group;
            grid.columns = uint32_t(columns);
            grid.rows = uint32_t(rows);
            grid.sizeFloor = float(active.sizeScale.floor);
            grid.codesPerOctave = float(active.sizeScale.codesPerOctave);
            const auto started = Clock::now();
            if (!binner_->bin(cb, grid, &error)) { qWarning("sentinel-lab bin: %s", qPrintable(error)); return; }
            telemetry_->binSubmitMs.store(msSince(started));
            telemetry_->rebins.fetch_add(1);
            telemetry_->gpuBytes.store(binner_->gpuBytes());
            lastGrid_ = grid;
        }
        const auto &g = *lastGrid_;
        const double firstTime = double(active.startMs / 60'000 + g.firstMinute) / tf;
        const double firstPrice = double(active.baseRow + g.rowLo) / group;
        DisplayMapping mapping;
        mapping.timeOffset = float(absoluteMinuteLo / tf - firstTime);
        mapping.timeSpan = float((absoluteMinuteHi - absoluteMinuteLo) / tf);
        mapping.priceOffset = float(firstPrice + g.rows - view_.priceHi / tick);
        mapping.priceSpan = float((view_.priceHi - view_.priceLo) / tick);
        telemetry_->gpuFrameMs.store(cb->lastCompletedGpuTime() * 1000.0);
        telemetry_->columns.store(g.columns); telemetry_->rows.store(g.rows);
        telemetry_->group.store(group); telemetry_->tick.store(tick);
        if (!binner_->draw(cb, renderTarget(), mapping, &error)) {
            qWarning("sentinel-lab draw: %s", qPrintable(error)); return;
        }
        if (telemetry_->firstFrameMs.load() == 0) telemetry_->firstFrameMs.store(msSince(launched));
        if (uploaded_ == data_) telemetry_->paintedVersion.store(version_);
        update();
    }
private:
    std::unique_ptr<GpuBinner> binner_;
    std::unique_ptr<GpuBinner> pending_;
    QRhi *resourceRhi_ = nullptr;
    std::shared_ptr<const recording::RecordingEntries> data_, uploaded_;
    std::shared_ptr<const recording::RecordingEntries> pendingData_;
    std::shared_ptr<Telemetry> telemetry_;
    LabItem::View view_;
    uint64_t version_ = 0;
    int timeframeMinutes_ = 1;
    bool realMode_ = false;
    double manualTick_ = 0;
    std::optional<Grid> lastGrid_;
    Clock::time_point previous_;
};

LabItem::LabItem(QQuickItem *parent) : QQuickRhiItem(parent) {
    setMirrorVertically(true);
}
LabItem::~LabItem() { loadGeneration_->fetch_add(1); }
QQuickRhiItemRenderer *LabItem::createRenderer() { return new LabRenderer; }
void LabItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickRhiItem::geometryChange(next, previous);
    if (!data_ || !(next.width() > 0)) return;
    const double maximum = next.width() * timeframeMinutes_ / minColumnPx_;
    const double span = view_.timeHi - view_.timeLo;
    if (span > maximum) {
        const double center = (view_.timeLo + view_.timeHi) * 0.5;
        view_.timeLo = center - maximum * 0.5;
        view_.timeHi = center + maximum * 0.5;
        ++version_; update();
    }
}

void LabItem::accept(std::shared_ptr<const recording::RecordingEntries> data, bool preserveView) {
    const auto previous = data_;
    data_ = std::move(data);
    if (!data_ || data_->nativeTick <= 0) { status_ = QStringLiteral("No source columns"); emit statusChanged(); return; }
    if (manualTick_ > 0 &&
        std::abs(manualTick_ / data_->nativeTick -
                 std::round(manualTick_ / data_->nativeTick)) > 1e-8) {
        manualTick_ = 0;
        emit tickChanged();
    }
    if (preserveView && previous && !previous->rowSide.empty()) {
        const double shift = double(previous->startMs - data_->startMs) / 60'000.0;
        view_.timeLo += shift;
        view_.timeHi += shift;
        const double newest = double(data_->columns()) * data_->sourceMinutes;
        if (view_.timeHi > newest) {
            const double extra = view_.timeHi - newest;
            view_.timeLo -= extra; view_.timeHi -= extra;
        }
        status_ = QStringLiteral("Ready: %1 entries").arg(data_->rowSide.size());
        ++version_; emit statusChanged(); update();
        return;
    }
    const double totalMinutes = double(data_->columns()) * data_->sourceMinutes;
    const double visible = std::min<double>(totalMinutes,
        std::max(1.0, width()) * timeframeMinutes_ / minColumnPx_);
    view_.timeHi = totalMinutes; view_.timeLo = view_.timeHi - visible;
    int64_t low = std::numeric_limits<int64_t>::max(), high = std::numeric_limits<int64_t>::min();
    for (const auto &c : data_->coverage) {
        if (c.bidLo <= c.bidHi) { low = std::min(low, data_->baseRow + c.bidLo); high = std::max(high, data_->baseRow + c.bidHi); }
        if (c.askLo <= c.askHi) { low = std::min(low, data_->baseRow + c.askLo); high = std::max(high, data_->baseRow + c.askHi); }
    }
    if (low >= high) { low = data_->baseRow; high = low + 100; }
    const double margin = std::max(2.0, (high - low) * 0.04);
    view_.priceLo = low * data_->nativeTick - margin;
    view_.priceHi = (high + 1) * data_->nativeTick + margin;
    status_ = QStringLiteral("Ready: %1 entries").arg(data_->rowSide.size());
    ++version_; emit statusChanged(); update();
}

void LabItem::loadReal(int hours, const QString &layer) {
    loadRealInternal(hours, layer, false);
}
void LabItem::loadRealInternal(int hours, const QString &layer, bool preserveView) {
    if (hours < 1 || hours > 24 * 30 || (layer != "near" && layer != "deep")) return;
    realHours_ = hours; realLayer_ = layer; realMode_ = true;
    const uint32_t sourceMinutes = sourceMinutesFor(timeframeMinutes_, layer);
    const int64_t sourceMs = int64_t(sourceMinutes) * 60'000;
    const auto generation = loadGeneration_->fetch_add(1) + 1;
    status_ = QStringLiteral("Loading %1 h %2...").arg(hours).arg(layer); emit statusChanged();
    const QPointer<LabItem> self(this);
    const auto guard = loadGeneration_;
    const auto mutex = loadMutex_;
    const auto telemetry = telemetry_;
    const auto firstPaintVersion = std::make_shared<std::atomic<uint64_t>>(std::numeric_limits<uint64_t>::max());
    const int64_t nowMinute = QDateTime::currentMSecsSinceEpoch() / 60'000 * 60'000;
    const int64_t end = sourceMinutes == 1 ? nowMinute :
        ((nowMinute + sourceMs - 1) / sourceMs) * sourceMs;
    const int64_t fullStart = (nowMinute - int64_t(hours) * 3'600'000) / sourceMs * sourceMs;
    const int64_t visibleStart = std::max(fullStart,
        (nowMinute - int64_t(std::min(hours, 2)) * 3'600'000) / sourceMs * sourceMs);
    QThreadPool::globalInstance()->start([self, guard, mutex, telemetry, firstPaintVersion,
                                          generation, layer, end, fullStart, visibleStart,
                                          sourceMinutes, preserveView] {
        std::scoped_lock lock(*mutex);
        if (guard->load() != generation) return;
        std::shared_ptr<recording::RecordingEntries> result;
        QString error;
        try {
            result = std::make_shared<recording::RecordingEntries>(
                sourceMinutes >= 60 && sourceMinutes % 60 == 0 && layer == "deep" ?
                recording::loadComposedHourEntries(
                    "/Volumes/T7/sentinel-data/recording", "BTC-USD", layer.toStdString(),
                    visibleStart, end, sourceMinutes) :
                recording::loadComposedMinuteEntries(
                    "/Volumes/T7/sentinel-data/recording", "BTC-USD", layer.toStdString(),
                    visibleStart, end, sourceMinutes));
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, guard, generation, result, error, firstPaintVersion, preserveView] {
            if (!self || guard->load() != generation) return;
            if (!error.isEmpty()) {
                self->status_ = unavailableStatus(error);
                emit self->statusChanged();
            }
            else {
                self->accept(result, preserveView);
                firstPaintVersion->store(self->version_);
            }
        }, Qt::QueuedConnection);
        if (!error.isEmpty() || fullStart == visibleStart || guard->load() != generation) return;
        const bool initialHasEntries = result && !result->rowSide.empty();
        const auto paintDeadline = Clock::now() + std::chrono::seconds(30);
        while (initialHasEntries && guard->load() == generation && Clock::now() < paintDeadline &&
               (firstPaintVersion->load() == std::numeric_limits<uint64_t>::max() ||
                telemetry->paintedVersion.load() < firstPaintVersion->load()))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (guard->load() != generation) return;
        if (initialHasEntries && telemetry->paintedVersion.load() < firstPaintVersion->load())
            qWarning("sentinel-lab: initial visible range did not paint within 30 s");
        const auto initial = result;
        try {
            auto older = result->sourceMinutes >= 60 &&
                         result->sourceMinutes % 60 == 0 && layer == "deep" ?
                recording::loadComposedHourEntries(
                    "/Volumes/T7/sentinel-data/recording", "BTC-USD", layer.toStdString(),
                    fullStart, visibleStart, result->sourceMinutes) :
                recording::loadComposedMinuteEntries(
                    "/Volumes/T7/sentinel-data/recording", "BTC-USD", layer.toStdString(),
                    fullStart, visibleStart, result->sourceMinutes);
            if (guard->load() != generation) return;
            if (older.sourceMinutes != initial->sourceMinutes)
                throw std::runtime_error("timeframe unavailable: background range has a different source resolution");
            result = std::make_shared<recording::RecordingEntries>(
                recording::joinRecordingEntries(older, *initial));
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, guard, generation, result, error] {
            if (!self || guard->load() != generation) return;
            if (!error.isEmpty()) {
                self->status_ = unavailableStatus(error);
                emit self->statusChanged();
            }
            else self->accept(result, true);
        }, Qt::QueuedConnection);
    });
}

void LabItem::loadSynthetic(int count) {
    if (count < 1 || count > 100'000'000) return;
    realMode_ = false;
    const auto generation = loadGeneration_->fetch_add(1) + 1;
    status_ = QStringLiteral("Generating %1 entries...").arg(count); emit statusChanged();
    const QPointer<LabItem> self(this);
    const auto guard = loadGeneration_;
    const auto mutex = loadMutex_;
    QThreadPool::globalInstance()->start([self, guard, mutex, generation, count] {
        std::scoped_lock lock(*mutex);
        if (guard->load() != generation) return;
        std::shared_ptr<recording::RecordingEntries> result;
        QString error;
        try { result = std::make_shared<recording::RecordingEntries>(recording::syntheticRecordingEntries(count)); }
        catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, guard, generation, result, error] {
            if (!self || guard->load() != generation) return;
            if (!error.isEmpty()) { self->status_ = error; emit self->statusChanged(); }
            else self->accept(result);
        }, Qt::QueuedConnection);
    });
}

void LabItem::pan(double dx, double dy) {
    if (!data_ || width() <= 0 || height() <= 0) return;
    const double dt = dx / width() * (view_.timeHi - view_.timeLo);
    const double dp = dy / height() * (view_.priceHi - view_.priceLo);
    view_.timeLo -= dt; view_.timeHi -= dt;
    view_.priceLo -= dp; view_.priceHi -= dp;
    ++version_; update();
}
void LabItem::setTimeframeMinutes(int minutes) {
    if (minutes < 1 || minutes > 1440 || timeframeMinutes_ == minutes) return;
    timeframeMinutes_ = minutes;
    if (data_ && width() > 0) {
        const double maximum = std::max(1.0, width()) * minutes / minColumnPx_;
        const double span = view_.timeHi - view_.timeLo;
        if (span > maximum) {
            const double center = (view_.timeLo + view_.timeHi) * 0.5;
            view_.timeLo = center - maximum * 0.5;
            view_.timeHi = center + maximum * 0.5;
        }
    }
    ++version_; emit timeframeChanged(); update();
    if (realMode_ && data_ && data_->sourceMinutes !=
        sourceMinutesFor(timeframeMinutes_, realLayer_))
        loadRealInternal(realHours_, realLayer_, true);
}
void LabItem::setAutoTimeframe(bool enabled) {
    if (autoTimeframe_ == enabled) return;
    autoTimeframe_ = enabled;
    if (enabled) {
        constexpr int timeframes[] = {1, 5, 15, 60, 240, 1440};
        int index = 0;
        while (index < 5 && timeframes[index] < timeframeMinutes_) ++index;
        if (timeframeMinutes_ != timeframes[index]) setTimeframeMinutes(timeframes[index]);
    }
    emit timeframeChanged();
}
void LabItem::setMinColumnPx(double pixels) {
    if (!std::isfinite(pixels) || pixels < 0.5 || pixels > 64 || pixels == minColumnPx_) return;
    minColumnPx_ = pixels;
    if (data_ && width() > 0) {
        const double maximum = std::max(1.0, width()) * timeframeMinutes_ / minColumnPx_;
        const double span = view_.timeHi - view_.timeLo;
        if (span > maximum) {
            const double center = (view_.timeLo + view_.timeHi) * 0.5;
            view_.timeLo = center - maximum * 0.5;
            view_.timeHi = center + maximum * 0.5;
        }
    }
    ++version_; emit timeframeChanged(); update();
}
void LabItem::setManualTick(double tick) {
    if (!std::isfinite(tick) || tick < 0 || tick == manualTick_) return;
    if (tick > 0 && data_ && data_->nativeTick > 0 &&
        std::abs(tick / data_->nativeTick - std::round(tick / data_->nativeTick)) > 1e-8) {
        status_ = QStringLiteral("Price tick must be a multiple of the native tick");
        emit statusChanged();
        emit tickChanged();
        return;
    }
    manualTick_ = tick;
    ++version_; emit tickChanged(); update();
}
void LabItem::zoom(double steps, bool priceOnly, double anchorX, double anchorY) {
    if (!data_) return;
    const double factor = std::exp(-steps * 0.12);
    if (!priceOnly) {
        const double span = view_.timeHi - view_.timeLo;
        double wanted = std::max(1.0, span * factor);
        int nextTf = timeframeMinutes_;
        if (autoTimeframe_) {
            constexpr int timeframes[] = {1, 5, 15, 60, 240, 1440};
            int index = 0;
            while (index < 5 && timeframes[index] < nextTf) ++index;
            if (factor > 1) {
                while (index < 5 && wanted > std::max(1.0, width()) * timeframes[index] / minColumnPx_)
                    ++index;
            } else {
                while (index > 0 && std::max(1.0, width()) * timeframes[index] / wanted > 8.0)
                    --index;
            }
            nextTf = timeframes[index];
        }
        wanted = std::min(wanted, std::max(1.0, width()) * nextTf / minColumnPx_);
        const double center = view_.timeLo + std::clamp(anchorX, 0.0, 1.0) * span;
        view_.timeLo = center - std::clamp(anchorX, 0.0, 1.0) * wanted;
        view_.timeHi = view_.timeLo + wanted;
        if (nextTf != timeframeMinutes_) {
            timeframeMinutes_ = nextTf;
            emit timeframeChanged();
            if (realMode_ && data_ && data_->sourceMinutes !=
                sourceMinutesFor(timeframeMinutes_, realLayer_))
                loadRealInternal(realHours_, realLayer_, true);
        }
    }
    const double priceSpan = view_.priceHi - view_.priceLo;
    const double desired = std::max(data_->nativeTick, priceSpan * factor);
    const double center = view_.priceLo + (1.0 - std::clamp(anchorY, 0.0, 1.0)) * priceSpan;
    view_.priceLo = center - (1.0 - std::clamp(anchorY, 0.0, 1.0)) * desired;
    view_.priceHi = view_.priceLo + desired;
    ++version_; update();
}
bool LabItem::saveScreenshot(const QString &path) {
    if (!window() || telemetry_->firstFrameMs.load() <= 0) return false;
    const QString target = path.isEmpty() ?
        QStringLiteral("screenshots/lab-%1.png").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")) :
        path;
    const QFileInfo file(target);
    const QString protectedRoot = QStringLiteral("/Volumes/T7/sentinel-data/recording");
    const QString canonicalParent = file.absoluteDir().canonicalPath();
    if (file.absoluteFilePath() == protectedRoot ||
        file.absoluteFilePath().startsWith(protectedRoot + "/") ||
        canonicalParent == protectedRoot || canonicalParent.startsWith(protectedRoot + "/"))
        return false;
    QDir().mkpath(file.absolutePath());
    return window()->grabWindow().save(target, "PNG");
}
QVariantMap LabItem::metrics() const {
    const double frameMs = telemetry_->frameMs.load();
    return {{"fps", frameMs > 0 ? 1000.0 / frameMs : 0.0}, {"frameMs", frameMs},
            {"binSubmitMs", telemetry_->binSubmitMs.load()}, {"gpuFrameMs", telemetry_->gpuFrameMs.load()},
            {"firstFrameMs", telemetry_->firstFrameMs.load()}, {"entries", data_ ? qlonglong(data_->rowSide.size()) : 0},
            {"gpuBytes", qulonglong(telemetry_->gpuBytes.load())},
            {"columns", telemetry_->columns.load()}, {"rows", telemetry_->rows.load()},
            {"group", telemetry_->group.load()}, {"tick", telemetry_->tick.load()},
            {"rebins", qulonglong(telemetry_->rebins.load())}, {"timeframeMinutes", timeframeMinutes_},
            {"loadMs", data_ ? data_->loadMs : 0}, {"decodeMs", data_ ? data_->decodeMs : 0}};
}
} // namespace lab
