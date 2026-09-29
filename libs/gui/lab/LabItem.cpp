#include "LabItem.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QQuickWindow>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <exception>

namespace lab {
namespace {
constexpr int kTimeframes[] = {1, 5, 15, 60, 240, 1440};
constexpr uint64_t kUploadBudgetBytes = 2ull << 20; // plan default: 2 MiB per frame
QString unavailableStatus(QString detail) { return QStringLiteral("Timeframe unavailable: %1").arg(detail.trimmed()); }
} // namespace

LabItem::LabItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
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

double LabItem::displayTick() const {
    if (manualTick_ > 0) return manualTick_;
    if (!source_.gpu || source_.gpu->ticks.empty()) return 0;
    const double finest = heatmap::gpu::commonTick(*source_.gpu); // every grid can build it
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    // Rows at least 2 physical pixels tall, on the price ladder.
    return heatmap::idealTick(view_.priceLo, view_.priceHi, std::max(1.0, height() * dpr), 2.0, finest,
                              source_.gpu->priceScale);
}

QSGNode *LabItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = old ? static_cast<heatmap::gpu::HeatmapRenderNode *>(old)
                     : new heatmap::gpu::HeatmapRenderNode(stats_);
    heatmap::gpu::HeatmapRenderNode::Frame frame;
    frame.source = source_.gpu;
    frame.view = view_;
    frame.displayTick = displayTick();
    frame.rect = QRectF(0, 0, width(), height());
    frame.uploadBudgetBytes = kUploadBudgetBytes;
    node->setFrame(std::move(frame));
    return node;
}

void LabItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickItem::geometryChange(next, previous);
    clampTimeSpan();
    update();
}

void LabItem::clampTimeSpan() {
    if (!source_.gpu || !(width() > 0)) return;
    const double maximum = width() * double(tfMs()) / minColumnPx_;
    const double span = view_.timeHiMs - view_.timeLoMs;
    if (span > maximum) {
        const double center = (view_.timeLoMs + view_.timeHiMs) * 0.5;
        view_.timeLoMs = center - maximum * 0.5;
        view_.timeHiMs = center + maximum * 0.5;
    }
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
        const double span = std::min(requested, std::max(1.0, width()) * double(tfMs()) / minColumnPx_);
        view_.timeHiMs = double(source_.endMs);
        view_.timeLoMs = view_.timeHiMs - span;
        double lo = gpu.coveredPriceLo, hi = gpu.coveredPriceHi;
        if (!(hi > lo)) { lo = 0; hi = 1; }
        const double center = source_.medianPrice > 0 ? source_.medianPrice : (lo + hi) / 2;
        const double half = std::min((hi - lo) / 2, center * 0.02);
        view_.priceLo = center - half;
        view_.priceHi = center + half;
    }
    const double common = heatmap::gpu::commonTick(gpu);
    if (manualTick_ > 0 && !(common > 0 && std::abs(manualTick_ / common - std::round(manualTick_ / common)) < 1e-8)) {
        manualTick_ = 0;
        emit tickChanged();
    }
    clampTimeSpan();
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
    clampTimeSpan();
    emit timeframeChanged();
    // The old source keeps drawing at its own timeframe until the new one is uploaded.
    if (source_.gpu) reload(true);
}
void LabItem::setAutoTimeframe(bool enabled) {
    if (autoTimeframe_ == enabled) return;
    autoTimeframe_ = enabled;
    if (enabled) {
        int index = 0;
        while (index < 5 && kTimeframes[index] < timeframeMinutes_) ++index;
        if (timeframeMinutes_ != kTimeframes[index]) setTimeframeMinutes(kTimeframes[index]);
    }
    emit timeframeChanged();
}
void LabItem::setMinColumnPx(double pixels) {
    if (!std::isfinite(pixels) || pixels < 0.5 || pixels > 64 || pixels == minColumnPx_) return;
    minColumnPx_ = pixels;
    clampTimeSpan();
    emit timeframeChanged();
    update();
}
void LabItem::setManualTick(double tick) {
    if (!std::isfinite(tick) || tick < 0 || tick == manualTick_) return;
    const double common = source_.gpu ? heatmap::gpu::commonTick(*source_.gpu) : 0;
    if (tick > 0 && common > 0 && std::abs(tick / common - std::round(tick / common)) >= 1e-8) {
        status_ = QStringLiteral("Price tick must be a multiple of $%1 (every native grid)").arg(common);
        emit statusChanged();
        emit tickChanged();
        return;
    }
    manualTick_ = tick;
    emit tickChanged();
    update();
}

void LabItem::zoom(double steps, bool priceOnly, double anchorX, double anchorY) {
    if (!source_.gpu) return;
    const double factor = std::exp(-steps * 0.12);
    if (!priceOnly) {
        const double span = view_.timeHiMs - view_.timeLoMs;
        double wanted = std::max(60'000.0, span * factor);
        int nextTf = timeframeMinutes_;
        const double widthPx = std::max(1.0, width());
        if (autoTimeframe_) {
            int index = 0;
            while (index < 5 && kTimeframes[index] < nextTf) ++index;
            if (factor > 1) {
                while (index < 5 && wanted > widthPx * kTimeframes[index] * 60'000.0 / minColumnPx_) ++index;
            } else {
                while (index > 0 && widthPx * kTimeframes[index] * 60'000.0 / wanted > 8.0) --index;
            }
            nextTf = kTimeframes[index];
        }
        wanted = std::min(wanted, widthPx * nextTf * 60'000.0 / minColumnPx_);
        const double a = std::clamp(anchorX, 0.0, 1.0);
        const double center = view_.timeLoMs + a * span;
        view_.timeLoMs = center - a * wanted;
        view_.timeHiMs = view_.timeLoMs + wanted;
        if (nextTf != timeframeMinutes_) setTimeframeMinutes(nextTf);
    }
    const double priceSpan = view_.priceHi - view_.priceLo;
    const double finest = heatmap::gpu::commonTick(*source_.gpu);
    const double desired = std::max(finest * 4, priceSpan * factor);
    const double a = 1.0 - std::clamp(anchorY, 0.0, 1.0);
    const double center = view_.priceLo + a * priceSpan;
    view_.priceLo = center - a * desired;
    view_.priceHi = view_.priceLo + desired;
    update();
}

bool LabItem::saveScreenshot(const QString &path) {
    if (!window() || !source_.gpu) return false;
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
    const auto *gpu = source_.gpu.get();
    return {{"fps", frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0}, {"frameMs", frameMs_},
            {"binSubmitMs", stats_->binSubmitMs.load()}, {"gpuFrameMs", stats_->gpuFrameMs.load()},
            {"firstFrameMs", firstFrameMs_}, {"entries", gpu ? qlonglong(gpu->entryCount) : 0},
            {"gpuBytes", qulonglong(stats_->gpuBytes.load())},
            {"columns", stats_->columns.load()}, {"rows", stats_->rows.load()},
            {"group", stats_->factor.load()}, {"tick", stats_->tick.load()},
            {"rebins", qulonglong(stats_->rebins.load())}, {"timeframeMinutes", timeframeMinutes_},
            {"uploadPending", stats_->uploadPending.load()}, {"errors", qulonglong(stats_->errors.load())},
            {"loadMs", source_.loadMs}, {"composeMs", source_.composeMs}, {"buildMs", source_.buildMs},
            {"settled", settled()}};
}
} // namespace lab
