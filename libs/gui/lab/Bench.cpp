#include "Bench.hpp"
#include "GpuBinner.hpp"
#include "servermodel/PriceLadder.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <algorithm>
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
        if (data.size.empty() || data.nativeTick <= 0) {
            print({{"error", QStringLiteral("source has no sparse entries")},
                   {"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
                   {"layer", layer}, {"hours", hours}, {"entries", double(data.size.size())},
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
                   {"hours", hours}, {"entries", double(data.size.size())},
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
                   {"hours", hours}, {"layer", layer}, {"entries", double(data.size.size())},
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
        std::vector<double> cpu, gpu;
        cpu.reserve(200); gpu.reserve(200);
        for (int i = 0; i < 200; ++i) {
            // Repeated full -> detail zooms exercise 1, 2, 5 and 10x bins.
            const double zoom = std::pow(2.0, double(i % 20) / 5.0);
            const double priceMid = (double(low) + high) * 0.5 * data.nativeTick;
            const double priceSpan = std::max(data.nativeTick * 64.0,
                                              (high - low + 1) * data.nativeTick / zoom);
            const double priceLo = priceMid - priceSpan * 0.5;
            const double tick = recording::ladderTick(priceSpan / 720.0 * 2.0,
                                                     data.nativeTick, data.priceScale);
            Grid grid;
            grid.timeLo = float(data.columns() * (1.0 - 1.0 / zoom) * 0.5);
            grid.timeHi = float(data.columns() - grid.timeLo);
            grid.columns = std::clamp<uint32_t>(uint32_t(grid.timeHi - grid.timeLo), 1, 1280);
            grid.group = static_cast<uint32_t>(std::llround(tick / data.nativeTick));
            const int64_t first = int64_t(std::floor(priceLo / tick)) * grid.group;
            if (first - data.baseRow < std::numeric_limits<int32_t>::min() ||
                first - data.baseRow > std::numeric_limits<int32_t>::max())
                return fail(QStringLiteral("bench row offset out of range"));
            grid.rowLo = int32_t(first - data.baseRow);
            grid.rows = std::clamp<uint32_t>(uint32_t(std::ceil((priceMid + priceSpan * 0.5 - first * data.nativeTick) / tick)), 1, 360);
            grid.sizeFloor = float(data.sizeScale.floor);
            grid.codesPerOctave = float(data.sizeScale.codesPerOctave);
            const auto started = Clock::now();
            if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
                return fail(QStringLiteral("begin bin frame failed"));
            const bool binned = binner.bin(cb, grid, &error);
            const bool drawn = !binned || i != 0 || binner.draw(cb, smokeTarget.get(), &error);
            const bool frameFinished = rhi->endOffscreenFrame() == QRhi::FrameOpSuccess;
            if (!binned || !drawn || !frameFinished)
                return fail(QStringLiteral("GPU bin failed: %1").arg(error));
            const double wall = elapsed(started);
            if (i >= 10) { cpu.push_back(wall); if (cb->lastCompletedGpuTime() > 0) gpu.push_back(cb->lastCompletedGpuTime() * 1000.0); }
        }
        std::sort(cpu.begin(), cpu.end()); std::sort(gpu.begin(), gpu.end());
        const auto &times = gpu.size() == cpu.size() ? gpu : cpu;
        print({{"source", synthetic ? QStringLiteral("synthetic") : QStringLiteral("real")},
               {"layer", synthetic ? QStringLiteral("synthetic") : layer},
               {"hours", hours}, {"entries", double(data.size.size())},
               {"load_ms", data.loadMs}, {"decode_ms", data.decodeMs}, {"upload_ms", uploadMs},
               {"gpu_bytes", double(binner.gpuBytes())}, {"source_gpu_bytes", double(binner.sourceBytes())},
               {"passes", 200}, {"fragment_smoke", true},
               {"timing", gpu.size() == cpu.size() ? QStringLiteral("GPU frame timestamp") : QStringLiteral("CPU finished offscreen frame")},
               {"bin_ms_p50", percentile(times, 0.50)}, {"bin_ms_p95", percentile(times, 0.95)},
               {"bin_ms_max", times.empty() ? 0 : times.back()}});
        return 0;
    } catch (const std::exception &e) {
        print({{"error", QString::fromUtf8(e.what())}, {"source", synthetic ? "synthetic" : "real"},
               {"hours", hours}, {"layer", layer}});
        return 2;
    }
}
} // namespace lab
