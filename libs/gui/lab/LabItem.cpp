#include "LabItem.hpp"
#include "servermodel/PriceLadder.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QThreadPool>
#include <rhi/qrhi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
const auto launched = Clock::now();
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
} // namespace

class LabRenderer final : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer *) override { binner_ = std::make_unique<GpuBinner>(rhi()); uploaded_.reset(); }
    void synchronize(QQuickRhiItem *item) override {
        auto *lab = static_cast<LabItem *>(item);
        data_ = lab->data_; view_ = lab->view_; version_ = lab->version_;
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
        if (uploaded_ != data_) {
            if (!binner_->upload(cb, *data_, &error)) { qWarning("sentinel-lab upload: %s", qPrintable(error)); return; }
            uploaded_ = data_;
            uploadedNow = true;
        }
        const auto size = renderTarget()->pixelSize();
        const double native = data_->nativeTick;
        const double height = std::max(1, size.height());
        const double pricePerPx = (view_.priceHi - view_.priceLo) / height;
        const double tick = recording::ladderTick(pricePerPx * 2.0, native, data_->priceScale);
        if (!(tick > 0)) return;
        const auto group = static_cast<uint32_t>(std::llround(tick / native));
        const int64_t firstRow = static_cast<int64_t>(std::floor(view_.priceLo / tick)) * group;
        const auto rows = std::clamp<uint32_t>(static_cast<uint32_t>(std::ceil(
            (view_.priceHi - double(firstRow) * native) / tick)), 1, uint32_t(std::max(1, size.height() / 2)));
        const auto cols = std::clamp<uint32_t>(static_cast<uint32_t>(std::floor(view_.timeHi - view_.timeLo)),
                                                1, uint32_t(std::max(1, size.width())));
        if (firstRow - data_->baseRow < std::numeric_limits<int32_t>::min() ||
            firstRow - data_->baseRow > std::numeric_limits<int32_t>::max()) return;
        Grid grid;
        grid.timeLo = float(view_.timeLo); grid.timeHi = float(view_.timeHi);
        grid.rowLo = int32_t(firstRow - data_->baseRow);
        grid.rowAlignment = int32_t((data_->baseRow % group + group) % group);
        grid.group = group; grid.columns = cols; grid.rows = rows;
        grid.sizeFloor = float(data_->sizeScale.floor);
        grid.codesPerOctave = float(data_->sizeScale.codesPerOctave);
        const bool gridChanged = !lastGrid_ || lastGrid_->columns != grid.columns ||
            lastGrid_->rows != grid.rows || lastGrid_->group != grid.group ||
            lastGrid_->rowLo != grid.rowLo || lastGrid_->timeLo != grid.timeLo ||
            lastGrid_->timeHi != grid.timeHi;
        if (uploadedNow || gridChanged || version_ != binnedVersion_) {
            const auto started = Clock::now();
            if (!binner_->bin(cb, grid, &error)) { qWarning("sentinel-lab bin: %s", qPrintable(error)); return; }
            telemetry_->binSubmitMs.store(msSince(started));
            telemetry_->gpuBytes.store(binner_->gpuBytes());
            lastGrid_ = grid;
            binnedVersion_ = version_;
        }
        telemetry_->gpuFrameMs.store(cb->lastCompletedGpuTime() * 1000.0);
        telemetry_->columns.store(cols); telemetry_->rows.store(rows);
        telemetry_->group.store(group); telemetry_->tick.store(tick);
        if (!binner_->draw(cb, renderTarget(), &error)) { qWarning("sentinel-lab draw: %s", qPrintable(error)); return; }
        if (telemetry_->firstFrameMs.load() == 0) telemetry_->firstFrameMs.store(msSince(launched));
        update();
    }
private:
    std::unique_ptr<GpuBinner> binner_;
    std::shared_ptr<const recording::RecordingEntries> data_, uploaded_;
    std::shared_ptr<Telemetry> telemetry_;
    LabItem::View view_;
    uint64_t version_ = 0;
    uint64_t binnedVersion_ = std::numeric_limits<uint64_t>::max();
    std::optional<Grid> lastGrid_;
    Clock::time_point previous_;
};

LabItem::LabItem(QQuickItem *parent) : QQuickRhiItem(parent) {
    setMirrorVertically(true);
}
LabItem::~LabItem() { loadGeneration_->fetch_add(1); }
QQuickRhiItemRenderer *LabItem::createRenderer() { return new LabRenderer; }

void LabItem::accept(std::shared_ptr<const recording::RecordingEntries> data) {
    data_ = std::move(data);
    if (!data_ || data_->nativeTick <= 0) { status_ = QStringLiteral("No source columns"); emit statusChanged(); return; }
    view_.timeLo = 0; view_.timeHi = data_->columns();
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
    if (hours < 1 || hours > 24 * 30 || (layer != "near" && layer != "deep")) return;
    const auto generation = loadGeneration_->fetch_add(1) + 1;
    status_ = QStringLiteral("Loading %1 h %2...").arg(hours).arg(layer); emit statusChanged();
    const QPointer<LabItem> self(this);
    const auto guard = loadGeneration_;
    const auto mutex = loadMutex_;
    const int64_t end = QDateTime::currentMSecsSinceEpoch() / 60'000 * 60'000;
    QThreadPool::globalInstance()->start([self, guard, mutex, generation, hours, layer, end] {
        std::scoped_lock lock(*mutex);
        if (guard->load() != generation) return;
        std::shared_ptr<recording::RecordingEntries> result;
        QString error;
        try {
            result = std::make_shared<recording::RecordingEntries>(recording::loadRecordingEntries(
                "/Volumes/T7/sentinel-data/recording", "BTC-USD", layer.toStdString(),
                end - int64_t(hours) * 3'600'000, end));
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, guard, generation, result, error] {
            if (!self || guard->load() != generation) return;
            if (!error.isEmpty()) { self->status_ = error; emit self->statusChanged(); }
            else self->accept(result);
        }, Qt::QueuedConnection);
    });
}

void LabItem::loadSynthetic(int count) {
    if (count < 1 || count > 100'000'000) return;
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
    view_.priceLo += dp; view_.priceHi += dp;
    ++version_; update();
}
void LabItem::zoom(double steps, bool priceOnly, double anchorX, double anchorY) {
    if (!data_) return;
    const double factor = std::exp(-steps * 0.12);
    auto scale = [factor](double &lo, double &hi, double anchor) {
        const double center = lo + std::clamp(anchor, 0.0, 1.0) * (hi - lo);
        lo = center + (lo - center) * factor; hi = center + (hi - center) * factor;
    };
    if (!priceOnly) scale(view_.timeLo, view_.timeHi, anchorX);
    scale(view_.priceLo, view_.priceHi, 1.0 - anchorY);
    if (view_.timeHi - view_.timeLo < 1) view_.timeHi = view_.timeLo + 1;
    if (view_.priceHi - view_.priceLo < data_->nativeTick) view_.priceHi = view_.priceLo + data_->nativeTick;
    ++version_; update();
}
QVariantMap LabItem::metrics() const {
    const double frameMs = telemetry_->frameMs.load();
    return {{"fps", frameMs > 0 ? 1000.0 / frameMs : 0.0}, {"frameMs", frameMs},
            {"binSubmitMs", telemetry_->binSubmitMs.load()}, {"gpuFrameMs", telemetry_->gpuFrameMs.load()},
            {"firstFrameMs", telemetry_->firstFrameMs.load()}, {"entries", data_ ? qlonglong(data_->rowSide.size()) : 0},
            {"gpuBytes", qulonglong(telemetry_->gpuBytes.load())},
            {"columns", telemetry_->columns.load()}, {"rows", telemetry_->rows.load()},
            {"group", telemetry_->group.load()}, {"tick", telemetry_->tick.load()},
            {"loadMs", data_ ? data_->loadMs : 0}, {"decodeMs", data_ ? data_->decodeMs : 0}};
}
} // namespace lab
