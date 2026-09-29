#include "Bench.hpp"
#include "GpuBinner.hpp"
#include "servermodel/PriceLadder.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double percentile(const std::vector<double> &v, double p) {
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, size_t(std::ceil(p * v.size()) - 1));
    return v[i];
}
void print(const QJsonObject &obj) {
    std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData() << '\n';
}
} // namespace

int runBench(int hours, const QString &layer, uint32_t synthetic) {
    try {
        recording::RecordingEntries data;
        if (synthetic) data = recording::syntheticRecordingEntries(synthetic);
        else {
            const int64_t end = QDateTime::currentMSecsSinceEpoch() / 60'000 * 60'000;
            data = recording::loadRecordingEntries("/Volumes/T7/sentinel-data/recording",
                                                   "BTC-USD", layer.toStdString(),
                                                   end - int64_t(hours) * 3'600'000, end);
        }
        if (data.rowSide.empty() || data.nativeTick <= 0) {
            print({{"error", QStringLiteral("source has no sparse entries")},
                   {"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
                   {"layer", layer}, {"hours", hours}, {"entries", double(data.rowSide.size())},
                   {"load_ms", data.loadMs}, {"decode_ms", data.decodeMs}});
            return 2;
        }
#ifdef Q_OS_MACOS
        // Qt's Metal QRhi aborts on a machine with no MTLDevice. Probe first so
        // sandboxed runs return a useful JSON result instead of SIGABRT.
        void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
        auto createDevice = metal ? reinterpret_cast<void *(*)()>(dlsym(metal, "MTLCreateSystemDefaultDevice")) : nullptr;
        const bool deviceAvailable = createDevice && createDevice();
        if (metal) dlclose(metal);
        if (!deviceAvailable) {
            print({{"error", QStringLiteral("No MTLDevice (Metal unavailable in this sandbox)")},
                   {"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
                   {"layer", synthetic ? QStringLiteral("synthetic") : layer},
                   {"hours", hours}, {"entries", double(data.rowSide.size())},
                   {"load_ms", data.loadMs}, {"decode_ms", data.decodeMs},
                   {"gpu_bytes_required", double(data.gpuBytes())}});
            return 2;
        }
        QRhiMetalInitParams init;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Metal, &init, QRhi::EnableTimestamps));
#else
        QRhiNullInitParams init;
        std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &init));
#endif
        if (!rhi) throw std::runtime_error("headless Metal QRhi creation failed");
        GpuBinner binner(rhi.get());
        QString error;
        auto fail = [&](const QString &message) {
            print({{"error", message}, {"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
                   {"hours", hours}, {"layer", layer}, {"entries", double(data.rowSide.size())},
                   {"load_ms", data.loadMs}, {"decode_ms", data.decodeMs}});
            return 2;
        };
        QRhiCommandBuffer *cb = nullptr;
        const auto uploadStart = Clock::now();
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
            return fail(QStringLiteral("begin upload frame failed"));
        const bool uploaded = binner.upload(cb, data, &error);
        const bool uploadFinished = rhi->endOffscreenFrame() == QRhi::FrameOpSuccess;
        if (!uploaded || !uploadFinished)
            return fail(QStringLiteral("GPU upload failed: %1").arg(error));
        const double uploadMs = elapsed(uploadStart);
        // Exercise the same fragment pipeline once offscreen. Timed passes
        // below remain compute-only; the first pass is excluded as warmup.
        std::unique_ptr<QRhiTexture> smokeColor(rhi->newTexture(
            QRhiTexture::RGBA8, QSize(32, 32), 1, QRhiTexture::RenderTarget));
        if (!smokeColor || !smokeColor->create()) return fail(QStringLiteral("smoke color texture failed"));
        std::unique_ptr<QRhiTextureRenderTarget> smokeTarget(rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(smokeColor.get()))));
        std::unique_ptr<QRhiRenderPassDescriptor> smokePass(smokeTarget->newCompatibleRenderPassDescriptor());
        smokeTarget->setRenderPassDescriptor(smokePass.get());
        if (!smokeTarget->create()) return fail(QStringLiteral("smoke render target failed"));
        int64_t low = std::numeric_limits<int64_t>::max(), high = std::numeric_limits<int64_t>::min();
        for (const auto &c : data.coverage) {
            if (c.bidLo <= c.bidHi) { low = std::min(low, data.baseRow + c.bidLo); high = std::max(high, data.baseRow + c.bidHi); }
            if (c.askLo <= c.askHi) { low = std::min(low, data.baseRow + c.askLo); high = std::max(high, data.baseRow + c.askHi); }
        }
        if (low >= high) return fail(QStringLiteral("source has no valid price coverage"));
        auto sweep = [&](uint32_t widthPx, uint32_t heightPx, int dpr) -> QJsonObject {
        struct ZoomRow {
            double zoom = 0;
            uint64_t visible = 0;
            uint32_t cols = 0, rows = 0, nativeRows = 0;
            double sourcePerOutput = 0;
            std::vector<double> cpu, gpu;
        };
        std::array<ZoomRow, 25> levels;
        std::vector<double> cpu, gpu;
        cpu.reserve(200); gpu.reserve(200);
        // QRhi::lastCompletedGpuTime belongs to the preceding finished frame
        // on Metal. Warm every level, time 200 passes, then submit one drain.
        for (int i = 0; i < 226; ++i) {
            const int levelIndex = i % int(levels.size());
            const double zoom = levelIndex < 20 ? std::pow(2.0, double(levelIndex) / 5.0) : 1.0;
            const double priceMid = (double(low) + high) * 0.5 * data.nativeTick;
            const double priceSpan = std::max(data.nativeTick * 64.0,
                                              (high - low + 1) * data.nativeTick / zoom);
            const double priceLo = priceMid - priceSpan * 0.5;
            const double tick = recording::ladderTick(priceSpan / double(heightPx) * 2.0,
                                                     data.nativeTick, data.priceScale);
            Grid grid;
            grid.timeLo = float(data.columns() * (1.0 - 1.0 / zoom) * 0.5);
            grid.timeHi = float(data.columns() - grid.timeLo);
            grid.columns = std::clamp<uint32_t>(uint32_t(grid.timeHi - grid.timeLo), 1, widthPx);
            if (levelIndex >= 20) grid.columns = std::min<uint32_t>(grid.columns, widthPx >> (levelIndex - 19));
            grid.group = static_cast<uint32_t>(std::llround(tick / data.nativeTick));
            const int64_t first = int64_t(std::floor(priceLo / tick)) * grid.group;
            if (first - data.baseRow < std::numeric_limits<int32_t>::min() ||
                first - data.baseRow > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("bench row offset out of range");
            grid.rowLo = int32_t(first - data.baseRow);
            if (data.baseRow < std::numeric_limits<int32_t>::min() ||
                data.baseRow > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("bench base row out of range");
            grid.baseRow = int32_t(data.baseRow);
            grid.rows = std::clamp<uint32_t>(uint32_t(std::ceil((priceMid + priceSpan * 0.5 - first * data.nativeTick) / tick)), 1, heightPx / 2);
            grid.sizeFloor = float(data.sizeScale.floor);
            grid.codesPerOctave = float(data.sizeScale.codesPerOctave);
            auto &level = levels[levelIndex];
            level.zoom = zoom; level.cols = grid.columns; level.rows = grid.rows;
            level.nativeRows = grid.group;
            level.sourcePerOutput = (grid.timeHi - grid.timeLo) / grid.columns;
            if (i < int(levels.size())) {
                const auto c0 = std::clamp<int>(int(std::ceil(grid.timeLo)), 0, data.columns());
                const auto c1 = std::clamp<int>(int(std::ceil(grid.timeHi)), 0, data.columns());
                const uint32_t row0 = uint32_t(std::max(grid.rowLo, 0));
                const uint32_t row1 = uint32_t(std::max<int64_t>(int64_t(grid.rowLo) + int64_t(grid.rows) * grid.group, 0));
                for (int c = c0; c < c1; ++c) {
                    const auto begin = data.rowSide.begin() + data.offsets[c];
                    const auto end = data.rowSide.begin() + data.offsets[c + 1];
                    auto lower = [&](uint32_t row) {
                        return std::lower_bound(begin, end, row, [](uint32_t packed, uint32_t key) {
                            return (packed & 0x7fffffffu) < key;
                        });
                    };
                    level.visible += lower(row1) - lower(row0);
                }
            }
            const auto started = Clock::now();
            if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
                throw std::runtime_error("begin bin frame failed");
            const bool binned = binner.bin(cb, grid, &error);
            const bool drawn = !binned || i != 0 || binner.draw(cb, smokeTarget.get(), &error);
            const bool frameFinished = rhi->endOffscreenFrame() == QRhi::FrameOpSuccess;
            if (!binned || !drawn || !frameFinished)
                throw std::runtime_error(QStringLiteral("GPU bin failed: %1").arg(error).toStdString());
            const double wall = elapsed(started);
            if (i >= 25 && i < 225) {
                cpu.push_back(wall); level.cpu.push_back(wall);
            }
            if (i >= 26 && cb->lastCompletedGpuTime() > 0) {
                const double ms = cb->lastCompletedGpuTime() * 1000.0;
                gpu.push_back(ms); levels[(i - 1) % levels.size()].gpu.push_back(ms);
            }
        }
        std::sort(cpu.begin(), cpu.end()); std::sort(gpu.begin(), gpu.end());
        const auto &times = gpu.size() == cpu.size() ? gpu : cpu;
        QJsonArray perZoom;
        std::cerr << "grid=" << widthPx << 'x' << heightPx << " dpr=" << dpr
                  << " zoom visible_entries grid rows_per_bin source_cols_per_output gpu_ms_p50 gpu_ms_p95 gpu_ms_max\n";
        for (auto &z : levels) {
            std::sort(z.cpu.begin(), z.cpu.end()); std::sort(z.gpu.begin(), z.gpu.end());
            const auto &t = z.gpu.size() == z.cpu.size() ? z.gpu : z.cpu;
            const double p50 = percentile(t, 0.5), p95 = percentile(t, 0.95);
            const double max = t.empty() ? 0 : t.back();
            std::cerr << z.zoom << ' ' << z.visible << ' ' << z.cols << 'x' << z.rows << ' '
                      << z.nativeRows << ' ' << z.sourcePerOutput << ' ' << p50 << ' ' << p95 << ' ' << max << '\n';
            perZoom.append(QJsonObject{{"zoom", z.zoom}, {"visible_entries", double(z.visible)},
                                       {"columns", int(z.cols)}, {"rows", int(z.rows)},
                                       {"native_rows_per_display_row", int(z.nativeRows)},
                                       {"source_columns_per_output_column", z.sourcePerOutput},
                                       {"bin_ms_p50", p50}, {"bin_ms_p95", p95}, {"bin_ms_max", max}});
        }
        return {{"width_px", int(widthPx)}, {"height_px", int(heightPx)}, {"dpr", dpr},
                {"timing", gpu.size() == cpu.size() ? QStringLiteral("GPU frame timestamp") : QStringLiteral("CPU finished offscreen frame")},
                {"bin_ms_p50", percentile(times, 0.50)}, {"bin_ms_p95", percentile(times, 0.95)},
                {"bin_ms_max", times.empty() ? 0 : times.back()}, {"zoom_levels", perZoom}};
        };
        const QJsonObject oneX = sweep(1920, 1080, 1);
        const QJsonObject twoX = sweep(3840, 2160, 2);
        const auto validColumns = std::count_if(data.observedMs.begin(), data.observedMs.end(),
                                                [](uint32_t ms) { return ms > 0; });
        print({{"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
               {"layer", synthetic ? QStringLiteral("synthetic") : layer},
               {"hours", hours}, {"entries", double(data.rowSide.size())},
               {"valid_source_columns", double(validColumns)},
               {"load_ms", data.loadMs}, {"decode_ms", data.decodeMs}, {"upload_ms", uploadMs},
               {"gpu_bytes", double(binner.gpuBytes())}, {"source_gpu_bytes", double(binner.sourceBytes())},
               {"source_bytes_per_million_entries", double(binner.sourceBytes()) * 1'000'000.0 / data.rowSide.size()},
               {"passes_per_grid", 200}, {"fragment_smoke", true},
               {"grid_1x", oneX}, {"grid_2x", twoX},
               {"bin_ms_p50", twoX.value("bin_ms_p50")}, {"bin_ms_p95", twoX.value("bin_ms_p95")},
               {"bin_ms_max", twoX.value("bin_ms_max")}});
        return 0;
    } catch (const std::exception &e) {
        print({{"error", QString::fromUtf8(e.what())}, {"source", synthetic ? "synthetic" : "real"},
               {"hours", hours}, {"layer", layer}});
        return 2;
    }
}
} // namespace lab
