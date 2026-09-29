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
        QRhiMetalInitParams init;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Metal, &init, QRhi::EnableTimestamps));
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

int runScreenshot(int hours, const QString &layer, uint32_t synthetic, int tfMinutes, const QString &path,
                  double panColumns) {
    const QString output = QFileInfo(path).absoluteFilePath();
    if (output.startsWith(QString::fromLatin1(kRecordingRoot))) {
        print({{"error", QStringLiteral("recording directory is read-only")}, {"screenshot", path}});
        return 2;
    }
    OffscreenQuick scene;
    QString error;
    if (!scene.create(QSize(1500, 880), &error)) {
        print({{"error", error}, {"screenshot", path}});
        return 2;
    }
    scene.window()->setColor(QColor(0x08, 0x0d, 0x12));
    auto *item = new LabItem;
    item->setParentItem(scene.window()->contentItem());
    item->setSize(QSizeF(1500, 880));
    item->setTimeframeMinutes(tfMinutes);
    if (synthetic) item->loadSynthetic(int(synthetic));
    else item->loadReal(hours, layer);
    QElapsedTimer timer;
    timer.start();
    QImage image;
    bool panned = panColumns == 0;
    int settledFrames = 0;
    while (timer.elapsed() < 120'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        item->update();
        image = scene.renderFrame(&error);
        if (image.isNull()) break;
        if (!item->settled()) { settledFrames = 0; continue; }
        if (!panned) { // exercise a sub-bin pan on the settled picture
            item->pan(-panColumns * item->width() / std::max(1.0, item->metrics().value("columns").toDouble()), 0);
            panned = true;
            continue;
        }
        if (++settledFrames >= 2) break;
    }
    const auto metrics = item->metrics();
    if (image.isNull() || !item->settled()) {
        print({{"error", image.isNull() ? error : QStringLiteral("source did not settle within 120 s: ") + item->status()},
               {"screenshot", path}});
        return 2;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!image.save(path, "PNG")) {
        print({{"error", QStringLiteral("PNG save failed")}, {"screenshot", path}});
        return 2;
    }
    QJsonObject result = QJsonObject::fromVariantMap(metrics);
    result.insert("screenshot", path);
    result.insert("status", item->status());
    result.insert("elapsed_ms", double(timer.elapsed()));
    print(result);
    return 0;
}
} // namespace lab
