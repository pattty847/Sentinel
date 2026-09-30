#include "Bench.hpp"
#include "LabItem.hpp"
#include "LabSources.hpp"
#include "OffscreenQuick.hpp"
#include "heatmap/HeatmapResolution.hpp"
#include "render/heatmap/HeatmapGpuBinner.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QQuickItem>
#include <QQuickWindow>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double percentile(const std::vector<double> &v, double p) {
    if (v.empty()) return 0;
    return v[std::min(v.size() - 1, size_t(std::ceil(p * double(v.size())) - 1))];
}
void print(const QJsonObject &obj) {
    std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData() << std::endl;
}
LabSource loadSource(int hours, const QString &layer, uint32_t synthetic, int tfMinutes) {
    const int64_t tf = int64_t(tfMinutes) * heatmap::kMinuteMs;
    return synthetic ? syntheticSource(synthetic, tf) : loadRealSource(layer.toStdString(), hours, hours, tf);
}
// Source entries whose bucket and native rows fall inside the grid.
uint64_t visibleEntries(const heatmap::gpu::GpuSource &s, const heatmap::gpu::BinGrid &grid) {
    const auto factors = heatmap::gpu::tickFactors(s, grid.displayTick);
    uint64_t total = 0;
    for (uint32_t x = 0; x < grid.columns; ++x) {
        const int64_t slot = grid.firstBucket + x - s.firstBucket;
        if (slot < 0 || slot >= int64_t(s.bucketSlots.size())) continue;
        const uint32_t c = s.bucketSlots[size_t(slot)];
        if (c >= s.columns()) continue;
        for (uint32_t g = s.columnGroups[c]; g < s.columnGroups[c + 1]; ++g) {
            const auto &meta = s.groups[g];
            const int64_t f = factors[meta.tickIndex];
            if (!f) continue;
            const int64_t lo = grid.firstBin * f - meta.baseRow, hi = (grid.firstBin + grid.rows) * f - meta.baseRow;
            auto rowOf = [&](uint32_t i) {
                const uint32_t key = s.wide ? s.entries[size_t(i) * 3 + 2] : (s.entries[size_t(i) * 2 + 1] & 0x1ffffu);
                return int64_t(key >> 1);
            };
            auto lower = [&](int64_t row) {
                uint32_t a = meta.entryBegin, b = meta.entryEnd;
                while (a < b) { const uint32_t m = (a + b) / 2; if (rowOf(m) < row) a = m + 1; else b = m; }
                return a;
            };
            total += lower(hi) - lower(lo);
        }
    }
    return total;
}
} // namespace

int runBench(int hours, const QString &layer, uint32_t synthetic, int tfMinutes) {
    const QString sourceName = synthetic ? QStringLiteral("synthetic") : QStringLiteral("real");
    try {
        const LabSource source = loadSource(hours, layer, synthetic, tfMinutes);
        const auto &gpu = *source.gpu;
        if (!gpu.entryCount || gpu.ticks.empty()) throw std::runtime_error("source has no entries");
        if (!metalDeviceAvailable()) {
            print({{"error", QStringLiteral("No MTLDevice (Metal unavailable in this sandbox)")},
                   {"source", sourceName}, {"layer", layer}, {"entries", double(gpu.entryCount)}});
            return 2;
        }
#ifdef Q_OS_MACOS
        QRhiMetalInitParams init;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Metal, &init, QRhi::EnableTimestamps));
#else
        std::unique_ptr<QRhi> rhi;
#endif
        if (!rhi) throw std::runtime_error("headless Metal QRhi creation failed");
        heatmap::gpu::HeatmapGpuBinner binner(rhi.get());
        // Default: the production choice (precision self-test, then fast kernel).
        if (qgetenv("SENTINEL_HEATMAP_KERNEL") == "precise") binner.forceKernel(heatmap::gpu::KernelVariant::Precise);
        QString error;
        QRhiCommandBuffer *cb = nullptr;
        const auto uploadStart = Clock::now();
        if (!binner.setSource(source.gpu, &error) || rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess ||
            !binner.uploadAll(cb, &error) || rhi->endOffscreenFrame() != QRhi::FrameOpSuccess)
            throw std::runtime_error(("GPU upload failed: " + error).toStdString());
        const double uploadMs = elapsed(uploadStart);
        const double finest = heatmap::gpu::commonTick(gpu);
        const double coveredLo = gpu.coveredPriceLo, coveredHi = gpu.coveredPriceHi;
        const double mid = source.medianPrice > 0 ? source.medianPrice : (coveredLo + coveredHi) / 2;
        const double fullSpan = std::min(coveredHi - coveredLo, 2 * std::min(mid - coveredLo, coveredHi - mid));
        const double tf = double(gpu.tfMs);
        const double totalMs = double(source.endMs - source.startMs);
        // Smoke-test the display pipeline once (timed passes are compute only).
        std::unique_ptr<QRhiTexture> color(rhi->newTexture(QRhiTexture::RGBA8, QSize(32, 32), 1, QRhiTexture::RenderTarget));
        if (!color->create()) throw std::runtime_error("smoke texture failed");
        std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget({QRhiColorAttachment(color.get())}));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create()) throw std::runtime_error("smoke target failed");

        auto sweep = [&](uint32_t widthPx, uint32_t heightPx, int dpr) -> QJsonObject {
            struct Level { double zoom = 0; uint64_t visible = 0; uint32_t cols = 0, rows = 0, factor = 0; std::vector<double> gpuMs, cpuMs; };
            std::array<Level, 25> levels;
            std::vector<double> gpuAll, cpuAll;
            // lastCompletedGpuTime reports the previous finished frame on Metal:
            // warm every level once, time 200 passes, then one drain frame.
            for (int i = 0; i < 226; ++i) {
                const int li = i % int(levels.size());
                const double zoom = std::pow(2.0, double(li) / 5.0);
                const double priceSpan = std::max(finest * 64, fullSpan / zoom);
                const double timeSpan = std::min(totalMs / zoom, double(widthPx) * tf); // >= 1 px per column
                heatmap::gpu::ViewWindow view;
                view.timeHiMs = double(source.endMs) - (totalMs - timeSpan) * 0.5;
                view.timeLoMs = view.timeHiMs - timeSpan;
                view.priceLo = mid - priceSpan / 2;
                view.priceHi = mid + priceSpan / 2;
                const double tick = heatmap::idealTick(view.priceLo, view.priceHi, heightPx, 2.0, finest, gpu.priceScale);
                const auto grid = heatmap::gpu::planGrid(view, gpu.tfMs, tick);
                if (!grid) throw std::runtime_error("bench grid out of range");
                auto &level = levels[size_t(li)];
                level.zoom = zoom; level.cols = grid->columns; level.rows = grid->rows;
                level.factor = heatmap::gpu::tickFactors(gpu, tick)[0];
                if (i == 0 && li == 0) {
                    std::cerr << "native ticks:";
                    for (const double t : gpu.ticks) std::cerr << ' ' << t;
                    std::cerr << " common " << finest << '\n';
                }
                if (i < int(levels.size())) level.visible = visibleEntries(gpu, *grid);
                const auto started = Clock::now();
                if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) throw std::runtime_error("begin frame failed");
                bool ok = binner.bin(cb, *grid, {}, &error);
                if (ok && i == 0) {
                    ok = binner.prepareDraw(pass.get(), 1, &error);
                    if (ok) {
                        auto *updates = rhi->nextResourceUpdateBatch();
                        binner.updateDraw(updates, QMatrix4x4(), QRectF(-1, -1, 2, 2), heatmap::gpu::mappingFor(*grid, view));
                        cb->beginPass(target.get(), Qt::black, {1.0f, 0}, updates);
                        cb->setViewport(QRhiViewport(0, 0, 32, 32));
                        cb->setScissor(QRhiScissor(0, 0, 32, 32));
                        binner.recordDraw(cb);
                        cb->endPass();
                    }
                }
                if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess || !ok)
                    throw std::runtime_error(("GPU bin failed: " + error).toStdString());
                const double wall = elapsed(started);
                if (i >= 25 && i < 225) { cpuAll.push_back(wall); level.cpuMs.push_back(wall); }
                if (i >= 26 && cb->lastCompletedGpuTime() > 0) {
                    const double ms = cb->lastCompletedGpuTime() * 1000.0;
                    gpuAll.push_back(ms);
                    levels[size_t((i - 1) % int(levels.size()))].gpuMs.push_back(ms);
                }
            }
            std::sort(gpuAll.begin(), gpuAll.end());
            std::sort(cpuAll.begin(), cpuAll.end());
            const bool gpuTimed = gpuAll.size() == cpuAll.size();
            const auto &times = gpuTimed ? gpuAll : cpuAll;
            QJsonArray perZoom;
            std::cerr << "grid " << widthPx << 'x' << heightPx << " (dpr " << dpr
                      << "): zoom visible_entries cols x rows rows_per_bin gpu_ms_p50 p95 max\n";
            for (auto &z : levels) {
                auto &t = gpuTimed ? z.gpuMs : z.cpuMs;
                std::sort(t.begin(), t.end());
                const double p50 = percentile(t, 0.5), p95 = percentile(t, 0.95), mx = t.empty() ? 0 : t.back();
                std::cerr << "  " << z.zoom << ' ' << z.visible << ' ' << z.cols << 'x' << z.rows << ' ' << z.factor
                          << ' ' << p50 << ' ' << p95 << ' ' << mx << '\n';
                perZoom.append(QJsonObject{{"zoom", z.zoom}, {"visible_entries", double(z.visible)},
                                           {"columns", int(z.cols)}, {"rows", int(z.rows)},
                                           {"native_rows_per_display_row", int(z.factor)},
                                           {"bin_ms_p50", p50}, {"bin_ms_p95", p95}, {"bin_ms_max", mx}});
            }
            return {{"width_px", int(widthPx)}, {"height_px", int(heightPx)}, {"dpr", dpr},
                    {"timing", gpuTimed ? QStringLiteral("GPU command buffer timestamp") : QStringLiteral("CPU wall per offscreen frame")},
                    {"bin_ms_p50", percentile(times, 0.5)}, {"bin_ms_p95", percentile(times, 0.95)},
                    {"bin_ms_max", times.empty() ? 0 : times.back()}, {"zoom_levels", perZoom}};
        };
        const QJsonObject oneX = sweep(1920, 1080, 1);
        const QJsonObject twoX = sweep(3840, 2160, 2);
        print({{"source", sourceName}, {"layer", synthetic ? QStringLiteral("synthetic") : layer},
               {"hours", hours}, {"timeframe_minutes", tfMinutes}, {"columns", double(gpu.columns())},
               {"entries", double(gpu.entryCount)}, {"sparse_entries", double(source.sparseEntries)},
               {"wide_entries", gpu.wide}, {"load_ms", source.loadMs}, {"compose_ms", source.composeMs},
               {"build_ms", source.buildMs}, {"upload_ms", uploadMs},
               {"source_gpu_bytes", double(binner.sourceBytes())}, {"gpu_bytes", double(binner.gpuBytes())},
               {"passes_per_grid", 200}, {"grid_1x", oneX}, {"grid_2x", twoX},
               {"kernel", binner.currentKernel() == heatmap::gpu::KernelVariant::Fast ? "fast" : "precise"},
               {"entry_pages", int(gpu.entryPages())},
               {"p95_1x_ms", oneX.value("bin_ms_p95")}, {"max_1x_ms", oneX.value("bin_ms_max")},
               {"p95_2x_ms", twoX.value("bin_ms_p95")}, {"max_2x_ms", twoX.value("bin_ms_max")}});
        return 0;
    } catch (const std::exception &e) {
        print({{"error", QString::fromUtf8(e.what())}, {"source", sourceName}, {"hours", hours}, {"layer", layer}});
        return 2;
    }
}

void applyTickOptions(LabItem &item, const LabRunOptions &options) {
    item.setHysteresis(options.hysteresis);
    item.setMinRowPx(options.minRowPx);
    item.setCrossfade(options.crossfade);
    if (options.tick > 0) item.setManualTick(options.tick);
    else if (options.manual) item.setManualMode(true);
    if (options.zoomRowsPx > 0) item.setInitialRowPx(options.zoomRowsPx);
}

namespace {
// Loads the source into a LabItem in an offscreen scene and renders until settled.
struct HeadlessLab {
    OffscreenQuick scene;
    LabItem *item = nullptr;
    QImage image;
    QString error;
    bool start(const LabRunOptions &options, QSize size) {
        if (!scene.create(size, &error)) return false;
        scene.window()->setColor(QColor(0x08, 0x0d, 0x12));
        item = new LabItem;
        item->setParentItem(scene.window()->contentItem());
        item->setSize(QSizeF(size));
        item->setTimeframeMinutes(options.tfMinutes);
        applyTickOptions(*item, options);
        if (options.synthetic) item->loadSynthetic(int(options.synthetic));
        else item->loadReal(options.hours, options.layer);
        return true;
    }
    bool frame() {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        item->update();
        image = scene.renderFrame(&error);
        return !image.isNull();
    }
    bool settle(qint64 timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        int settledFrames = 0;
        while (timer.elapsed() < timeoutMs) {
            if (!frame()) return false;
            if (!item->settled()) { settledFrames = 0; continue; }
            if (++settledFrames >= 3) return true;
        }
        error = QStringLiteral("source did not settle within %1 s: %2").arg(timeoutMs / 1000).arg(item->status());
        return false;
    }
};

QString money(double v) { return QStringLiteral("$") + QString::number(v, 'g', 12); }

// Debug state stamped on screenshots (the QML panel is not part of the headless scene).
void annotate(QImage &image, const QVariantMap &m, const QString &indicator) {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font(QStringLiteral("Menlo"));
    font.setPixelSize(15);
    painter.setFont(font);
    const QString line = QStringLiteral("mode=%1  tick=%2  h=%3  minRowPx=%4  commonTick(view)=%5  rows=%6px  "
                                        "tf=%7m  re-bin CPU submit=%8 ms  bins=%9  tick changes=%10")
        .arg(m.value("mode").toString(), money(m.value("tick").toDouble()))
        .arg(m.value("hysteresis").toDouble()).arg(m.value("minRowPx").toDouble())
        .arg(money(m.value("commonTick").toDouble()))
        .arg(m.value("rowPx").toDouble(), 0, 'f', 2).arg(m.value("timeframeMinutes").toInt())
        .arg(m.value("binSubmitMs").toDouble(), 0, 'f', 3).arg(m.value("rebins").toULongLong())
        .arg(m.value("tickChanges").toULongLong());
    const QFontMetrics fm(font);
    auto box = [&](int y, const QString &text, QColor color) {
        const QRect r(8, y, fm.horizontalAdvance(text) + 16, fm.height() + 8);
        painter.fillRect(r, QColor(16, 24, 32, 220));
        painter.setPen(color);
        painter.drawText(r.adjusted(8, 4, -8, -4), Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    box(8, line, QColor(0xa6, 0xe7, 0xe9));
    if (!indicator.isEmpty()) box(8 + fm.height() + 14, indicator, QColor(0xf0, 0xb4, 0x6a));
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

int runScreenshot(const LabRunOptions &options, const QString &path) {
    const QString output = QFileInfo(path).absoluteFilePath();
    if (output.startsWith(QString::fromLatin1(kRecordingRoot))) {
        print({{"error", QStringLiteral("recording directory is read-only")}, {"screenshot", path}});
        return 2;
    }
    HeadlessLab lab;
    if (!lab.start(options, QSize(1500, 880)) || !lab.settle(120'000)) {
        print({{"error", lab.error}, {"screenshot", path}});
        return 2;
    }
    if (options.panColumns != 0) { // exercise a sub-bin pan on the settled picture
        auto *item = lab.item;
        item->pan(-options.panColumns * item->width() / std::max(1.0, item->metrics().value("columns").toDouble()), 0);
        for (int i = 0; i < 2; ++i)
            if (!lab.frame()) { print({{"error", lab.error}, {"screenshot", path}}); return 2; }
    }
    const auto metrics = lab.item->metrics();
    QImage image = lab.image.convertToFormat(QImage::Format_RGBA8888);
    annotate(image, metrics, lab.item->resolutionIndicator());
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!image.save(path, "PNG")) {
        print({{"error", QStringLiteral("PNG save failed")}, {"screenshot", path}});
        return 2;
    }
    QJsonObject result = QJsonObject::fromVariantMap(metrics);
    result.insert("screenshot", path);
    result.insert("status", lab.item->status());
    print(result);
    return 0;
}

int runTickSweep(const LabRunOptions &options) {
    HeadlessLab lab;
    LabRunOptions base = options;
    base.manual = false;
    base.tick = 0;
    base.zoomRowsPx = 0;
    base.crossfade = false;
    if (!lab.start(base, QSize(1500, 880)) || !lab.settle(120'000)) {
        print({{"error", lab.error}});
        return 2;
    }
    auto *item = lab.item;
    const std::vector<double> hs = options.hysteresisSet ? std::vector<double>{options.hysteresis}
                                                         : std::vector<double>{0, 0.15, 0.25, 0.4};
    // Row heights of one commonTick() row, physical px: 0.02 .. 40, 3 % per step
    // (a fine trackpad zoom; a wheel notch is 12 %).
    constexpr double kStep = 1.03, kLo = 0.02, kHi = 40;
    QJsonArray summaries;
    for (const double h : hs) {
        item->setHysteresis(h);
        item->zoomToRowPx(kLo);
        for (int i = 0; i < 3; ++i) if (!lab.frame()) return 2;
        struct Change { double rowPx; double to; };
        std::vector<Change> inChanges;
        std::vector<double> binMs, gpuMs;
        int outChanges = 0, jitterFlips = 0;
        bool monotonic = true;
        double tick = item->metrics().value("tick").toDouble();
        // 0 unchanged, -1 finer, +1 coarser, 2 render failure.
        auto step = [&](double px, const char *phase) -> int {
            item->zoomToRowPx(px);
            if (!lab.frame()) return 2;
            const auto m = item->metrics();
            const double next = m.value("tick").toDouble();
            if (next == tick) return 0;
            const double ms = m.value("tickChangeBinMs").toDouble();
            if (!lab.frame()) return 2; // Metal reports the previous frame's GPU time
            const double gpu = item->metrics().value("gpuFrameMs").toDouble();
            binMs.push_back(ms);
            gpuMs.push_back(gpu);
            print({{"h", h}, {"phase", phase}, {"common_row_px", px}, {"from", tick}, {"to", next},
                   {"new_row_px", m.value("rowPx").toDouble()}, {"rebin_submit_ms", ms}, {"gpu_frame_ms", gpu}});
            const int direction = next < tick ? -1 : 1;
            tick = next;
            return direction;
        };
        for (double px = kLo; px <= kHi; px *= kStep) {
            const int d = step(px, "in");
            if (d == 2) return 2;
            if (d == 1) monotonic = false;
            if (d == -1) inChanges.push_back({px, tick});
        }
        for (double px = kHi; px >= kLo / 3; px /= kStep) {
            const int d = step(px, "out");
            if (d == 2) return 2;
            if (d == -1) monotonic = false;
            if (d == 1) ++outChanges;
        }
        // Jitter one step either side of every zoom-in change point.
        for (const auto &c : inChanges) {
            item->zoomToRowPx(c.rowPx);
            if (!lab.frame()) return 2;
            tick = item->metrics().value("tick").toDouble();
            for (int i = 0; i < 10; ++i)
                for (const double px : {c.rowPx * kStep, c.rowPx, c.rowPx / kStep, c.rowPx}) {
                    const int d = step(px, "jitter");
                    if (d == 2) return 2;
                    jitterFlips += d != 0;
                }
        }
        const QJsonObject summary{{"h", h}, {"changes_in", int(inChanges.size())}, {"changes_out", outChanges},
                                  {"jitter_flips", jitterFlips}, {"monotonic", monotonic},
                                  {"rebin_submit_ms_median", median(binMs)},
                                  {"rebin_submit_ms_max", binMs.empty() ? 0 : *std::max_element(binMs.begin(), binMs.end())},
                                  {"gpu_frame_ms_median", median(gpuMs)},
                                  {"gpu_frame_ms_max", gpuMs.empty() ? 0 : *std::max_element(gpuMs.begin(), gpuMs.end())}};
        print(QJsonObject{{"summary", summary}});
        summaries.append(summary);
    }
    const auto m = item->metrics();
    print({{"sweep", summaries}, {"source", options.synthetic ? QStringLiteral("synthetic") : options.layer},
           {"hours", options.hours}, {"timeframe_minutes", options.tfMinutes}, {"min_row_px", options.minRowPx},
           {"step", kStep}, {"errors", m.value("errors").toDouble()}});
    return 0;
}
int runTickChangeSequence(const LabRunOptions &options, const QString &dir) {
    if (QFileInfo(dir).absoluteFilePath().startsWith(QString::fromLatin1(kRecordingRoot))) {
        print({{"error", QStringLiteral("recording directory is read-only")}});
        return 2;
    }
    HeadlessLab lab;
    if (!lab.start(options, QSize(1500, 880)) || !lab.settle(120'000)) {
        print({{"error", lab.error}});
        return 2;
    }
    QDir().mkpath(dir);
    auto *item = lab.item;
    const double before = item->metrics().value("tick").toDouble();
    // Common-tick rows shrink 5x: Auto rows fall below minRowPx * (1 - h) for any h <= 0.4.
    const double common = item->metrics().value("commonTick").toDouble();
    const double rowPx = item->metrics().value("rowPx").toDouble() * common / std::max(before, 1e-12);
    item->zoomToRowPx(rowPx / 5);
    QElapsedTimer timer;
    timer.start();
    QJsonArray frames;
    int next = 0;
    while (timer.elapsed() <= 300) {
        if (!lab.frame()) { print({{"error", lab.error}}); return 2; }
        const qint64 ms = timer.elapsed();
        if (ms < next) continue;
        const auto m = item->metrics();
        QImage image = lab.image.convertToFormat(QImage::Format_RGBA8888);
        annotate(image, m, QStringLiteral("t=%1 ms after the zoom  crossfading=%2")
                               .arg(ms).arg(m.value("crossfading").toBool() ? "yes" : "no"));
        const QString path = QStringLiteral("%1/tick-change-%2ms.png").arg(dir).arg(ms, 3, 10, QLatin1Char('0'));
        if (!image.save(path, "PNG")) { print({{"error", QStringLiteral("PNG save failed")}}); return 2; }
        frames.append(QJsonObject{{"ms", double(ms)}, {"tick", m.value("tick").toDouble()},
                                  {"crossfading", m.value("crossfading").toBool()}, {"path", path}});
        next = int(ms) + 25;
    }
    print({{"from_tick", before}, {"to_tick", item->metrics().value("tick").toDouble()},
           {"crossfade_ms", options.crossfade ? LabItem::kCrossfadeMs : 0.0}, {"frames", frames}});
    return 0;
}
} // namespace lab
