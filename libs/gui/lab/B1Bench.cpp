#include "B1Bench.hpp"
#include "LabChunks.hpp"
#include "LabItem.hpp"
#include "OffscreenQuick.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double kFrameMs = 1000.0 / 60.0; // paced like a 60 Hz window
constexpr double kTimeoutMs = 120'000;
double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
double mib(double bytes) { return bytes / double(1 << 20); }

struct Scene {
    std::unique_ptr<OffscreenQuick> quick = std::make_unique<OffscreenQuick>();
    std::vector<LabItem *> items;
    QString error;
    double lastFrameMs = 0; // CPU wall of the last frame, GPU completion included
    bool create(QSize itemSize, int count) {
        const int columns = count > 1 ? 2 : 1, rows = (count + 1) / 2;
        if (!quick->create(QSize(itemSize.width() * columns, itemSize.height() * rows), &error)) return false;
        quick->window()->setColor(QColor(0x08, 0x0d, 0x12));
        for (int i = 0; i < count; ++i) {
            auto *item = new LabItem(quick->window()->contentItem());
            item->setPosition(QPointF((i % 2) * itemSize.width(), (i / 2) * itemSize.height()));
            item->setSize(QSizeF(itemSize));
            items.push_back(item);
        }
        return true;
    }
    bool frame() {
        const auto start = Clock::now();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        for (auto *item : items) item->update();
        if (!quick->renderFrameOnly(&error)) return false;
        lastFrameMs = since(start);
        const double remaining = kFrameMs - since(start);
        if (remaining > 1) {
            QEventLoop loop;
            QTimer::singleShot(int(remaining), &loop, &QEventLoop::quit);
            loop.exec();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        return true;
    }
    ~Scene() {
        for (auto *item : items) delete item;
        items.clear();
        quick.reset();
    }
};

struct Wait {
    bool ok = false;
    double ms = 0;
    int frames = 0;
    double firstFrameMs = 0, maxFrameMs = 0;
    QJsonObject json() const {
        return {{"ok", ok}, {"ms", ms}, {"frames", frames}, {"first_frame_ms", firstFrameMs}, {"max_frame_ms", maxFrameMs}};
    }
};
// Renders frames until done() after at least one frame; latency from `start`.
Wait waitFor(Scene &scene, const std::function<bool()> &done, Clock::time_point start, double timeoutMs = kTimeoutMs) {
    Wait w;
    while (since(start) < timeoutMs) {
        if (!scene.frame()) return w;
        ++w.frames;
        if (w.frames == 1) w.firstFrameMs = scene.lastFrameMs;
        w.maxFrameMs = std::max(w.maxFrameMs, scene.lastFrameMs);
        if (done()) {
            w.ok = true;
            w.ms = since(start);
            return w;
        }
    }
    w.ms = since(start);
    return w;
}
double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(std::ceil(p * double(v.size())) - 1))];
}
QJsonObject series(const std::vector<Wait> &waits, const std::vector<int> &rebuilds) {
    std::vector<double> ms, frames, first;
    int failed = 0, rebuilt = 0;
    for (const auto &w : waits) {
        ms.push_back(w.ms);
        frames.push_back(w.frames);
        first.push_back(w.firstFrameMs);
        failed += !w.ok;
    }
    for (const int r : rebuilds) rebuilt += r > 0;
    return {{"steps", int(waits.size())}, {"failed", failed}, {"ms_p50", percentile(ms, 0.5)},
            {"ms_p95", percentile(ms, 0.95)}, {"ms_max", percentile(ms, 1)}, {"frames_p50", percentile(frames, 0.5)},
            {"frames_max", percentile(frames, 1)}, {"input_frame_ms_p50", percentile(first, 0.5)},
            {"input_frame_ms_max", percentile(first, 1)}, {"steps_with_rebuild", rebuilt}};
}

struct Case {
    PrepMode mode;
    int dpr;
    QString layer;
    int tfMinutes;
    double tick;
};

QSize itemPixels(int dpr) { return dpr == 2 ? QSize(3000, 1760) : QSize(1500, 880); }

// The session's view: newest data at the right edge, 3 physical px per column
// (all the data when it is shorter), rows of the fixed tick 3 physical px tall.
heatmap::gpu::ViewWindow sessionView(const Case &c, const LayerInfo &info, double price) {
    const QSize px = itemPixels(c.dpr);
    const int64_t tf = int64_t(c.tfMinutes) * heatmap::kMinuteMs;
    const auto &a = info.availability;
    const double end = double(recording::floorDiv(a.endMs + tf - 1, tf) * tf);
    const double span = std::min(double(px.width() / 3) * double(tf), end - double(a.oldestMs));
    const double priceSpan = double(px.height()) / 3.0 * c.tick;
    return {end - span, end, price - priceSpan / 2, price + priceSpan / 2};
}

void drainWorkers() { QThreadPool::globalInstance()->waitForDone(); }

double metric(LabItem *item, const char *key) { return item->metrics().value(key).toDouble(); }

QJsonObject memoryOf(LabItem *item) {
    const auto m = item->metrics();
    return {{"gpu_bytes", m.value("gpuBytes").toDouble()},
            {"prep_cpu_bytes", m.value("prepCpuBytes").toDouble()},
            {"chunk_store_bytes", m.value("chunkBytes").toDouble()},
            {"intermediate_bytes", m.value("interBytes").toDouble()},
            {"footprint_bytes", m.value("footprintBytes").toDouble()},
            {"binner_bytes", m.value("binnerBytes").toDouble()},
            {"tile_gpu_bytes", m.value("tileGpuBytes").toDouble()},
            {"tiles_resident", m.value("tilesResident").toDouble()}};
}

QJsonObject runSession(const Case &c) {
    QJsonObject out{{"mode", prepModeName(c.mode)}, {"dpr", c.dpr}, {"layer", c.layer}, {"tf_minutes", c.tfMinutes},
                    {"tick", c.tick}};
    const std::string layer = c.layer.toStdString();
    const auto info = layerInfo(layer, true);
    if (!info.error.empty()) { out["error"] = QString::fromStdString(info.error); return out; }
    const double price = recentMedianPrice(layer);
    const auto view = sessionView(c, info, price);
    out["view_minutes"] = (view.timeHiMs - view.timeLoMs) / 60'000.0;
    out["view_price_span"] = view.priceHi - view.priceLo;
    const QSize px = itemPixels(c.dpr);
    auto makeScene = [&](Scene &scene) {
        if (!scene.create(px, 1)) return false;
        auto *item = scene.items[0];
        item->setTimeframeMinutes(c.tfMinutes);
        item->setPrepMode(c.mode);
        item->setManualTick(c.tick);
        item->setInitialView(view);
        return true;
    };
    // 1. Cold: nothing decoded, no tile intermediates.
    drainWorkers();
    chunkStore().clear();
    clearIntermediates();
    {
        Scene scene;
        if (!makeScene(scene)) { out["error"] = scene.error; return out; }
        auto *item = scene.items[0];
        const auto start = Clock::now();
        item->loadReal(24, c.layer);
        const auto w = waitFor(scene, [&] { return item->settled(); }, start);
        out["ttfv_cold"] = w.json();
        out["cold_chunk_loads"] = metric(item, "chunkLoads");
        out["cold_chunk_load_ms"] = metric(item, "chunkLoadMs");
        drainWorkers();
    }
    // 2. Warm: another chart already decoded the chunks (and W's intermediates).
    chunkStore().resetStats();
    resetIntermediateStats();
    Scene scene;
    if (!makeScene(scene)) { out["error"] = scene.error; return out; }
    auto *item = scene.items[0];
    auto settled = [&] { return item->settled(); };
    auto builds = [&] { return int(metric(item, "prepBuilds")); };
    double peakGpu = 0;
    auto trackPeak = [&] { peakGpu = std::max(peakGpu, metric(item, "gpuBytes")); };
    {
        const auto start = Clock::now();
        item->loadReal(24, c.layer);
        const auto w = waitFor(scene, settled, start);
        out["ttfv_warm"] = w.json();
        const auto m = item->metrics();
        out["initial"] = QJsonObject{{"prep_ms", m.value("lastPrepMs").toDouble()},
                                     {"builds", m.value("prepBuilds").toDouble()},
                                     {"upload_bytes", m.value("prepUploadBytes").toDouble()},
                                     {"source_entries", m.value("prepSourceEntries").toDouble()},
                                     {"tiles", m.value("tiles").toDouble()},
                                     {"memory", memoryOf(item)}};
        if (!w.ok) { out["error"] = QStringLiteral("warm start did not settle: ") + item->status(); return out; }
    }
    trackPeak();
    auto step = [&](const std::function<void()> &input, int *rebuilt = nullptr) {
        const int before = builds();
        const auto start = Clock::now();
        input();
        const auto w = waitFor(scene, settled, start);
        trackPeak();
        if (rebuilt) *rebuilt = builds() - before;
        return w;
    };
    auto reset = [&] {
        item->setView(view);
        waitFor(scene, settled, Clock::now());
    };
    const double width = item->width(), height = item->height();
    // 3. Small vertical pans (5 % of the view height each).
    {
        std::vector<Wait> waits;
        std::vector<int> rebuilds;
        for (int i = 0; i < 20; ++i) {
            int r = 0;
            waits.push_back(step([&] { item->pan(0, 0.05 * height); }, &r));
            rebuilds.push_back(r);
        }
        out["vpan_small"] = series(waits, rebuilds);
    }
    // 4. One large vertical pan (3 view heights): leaves V's prepared rows.
    {
        int r = 0;
        const auto w = step([&] { item->pan(0, 3 * height); }, &r);
        auto j = w.json();
        j["rebuilds"] = r;
        out["vpan_large"] = j;
    }
    reset();
    // 5. Small horizontal pans (5 % of the width, back in time).
    {
        std::vector<Wait> waits;
        std::vector<int> rebuilds;
        for (int i = 0; i < 20; ++i) {
            int r = 0;
            waits.push_back(step([&] { item->pan(0.05 * width, 0); }, &r));
            rebuilds.push_back(r);
        }
        out["hpan_small"] = series(waits, rebuilds);
    }
    // 6. One large horizontal pan into unprepared (and undecoded) chunks: 3 views
    // back, or as far as the recording goes.
    {
        const auto v = item->view();
        const double span = v.timeHiMs - v.timeLoMs;
        const double shift = std::min(3 * span, v.timeLoMs - double(info.availability.oldestMs));
        if (shift >= span) {
            const double loads = metric(item, "chunkLoads"), loadMs = metric(item, "chunkLoadMs");
            int r = 0;
            const auto w = step([&] { item->pan(shift / span * width, 0); }, &r);
            auto j = w.json();
            j["rebuilds"] = r;
            j["views"] = shift / span;
            j["chunk_loads"] = metric(item, "chunkLoads") - loads;
            j["chunk_load_ms"] = metric(item, "chunkLoadMs") - loadMs;
            out["hpan_large"] = j;
        } else {
            out["hpan_large"] = QJsonObject{{"skipped", QStringLiteral("recording shorter than 2 views")}};
        }
    }
    reset();
    // 7. Auto tick: price zoom-out until the tick changes; latency of that step.
    {
        item->setManualMode(false);
        waitFor(scene, settled, Clock::now());
        const double from = metric(item, "tick");
        QJsonObject j{{"from_tick", from}};
        for (int i = 0; i < 40; ++i) {
            int r = 0;
            const auto w = step([&] { item->zoom(-1, true, 0.5, 0.5); }, &r);
            const double tick = metric(item, "tick");
            if (tick != from) {
                j = w.json();
                j["from_tick"] = from;
                j["to_tick"] = tick;
                j["zoom_steps"] = i + 1;
                j["rebuilds"] = r;
                break;
            }
        }
        out["zoom_tick_change"] = j;
        item->setManualTick(c.tick);
    }
    reset();
    // 8. A revised newest chunk (re-read from the live recording).
    {
        const auto store0 = chunkStore().stats().revisions;
        const int before = builds();
        const auto start = Clock::now();
        item->reviseNewestChunk();
        int framesAfter = -1;
        const auto w = waitFor(scene, [&] {
            if (chunkStore().stats().revisions == store0) return false;
            return ++framesAfter >= 1 && item->settled();
        }, start);
        auto j = w.json();
        j["rebuilds"] = builds() - before;
        j["prep_ms"] = metric(item, "lastPrepMs");
        out["revise"] = j;
    }
    trackPeak();
    const auto m = item->metrics();
    const double hits = m.value("chunkHits").toDouble(), misses = m.value("chunkMisses").toDouble();
    out["cache"] = QJsonObject{{"chunk_hits", hits}, {"chunk_misses", misses},
                               {"chunk_hit_rate", hits + misses > 0 ? hits / (hits + misses) : 0.0},
                               {"chunk_loads", m.value("chunkLoads").toDouble()},
                               {"tile_hits", m.value("tileHits").toDouble()},
                               {"tile_misses", m.value("tileMisses").toDouble()},
                               {"tile_evictions", m.value("tileEvictions").toDouble()},
                               {"intermediate_hits", m.value("interHits").toDouble()},
                               {"intermediate_misses", m.value("interMisses").toDouble()},
                               {"builds", m.value("prepBuilds").toDouble()},
                               {"upload_bytes", m.value("prepUploadBytes").toDouble()},
                               {"clipped_tiles", m.value("clippedTiles").toDouble()}};
    auto memory = memoryOf(item);
    memory["gpu_bytes_peak"] = peakGpu;
    out["memory"] = memory;
    out["errors"] = m.value("errors").toDouble();
    drainWorkers();
    return out;
}

QJsonObject runMultiChart(PrepMode mode, const QString &layer, int count) {
    static const int tfs[] = {1, 5, 60, 15};
    drainWorkers();
    chunkStore().clear();
    clearIntermediates();
    chunkStore().resetStats();
    resetIntermediateStats();
    QJsonObject out{{"mode", prepModeName(mode)}, {"layer", layer}, {"charts", count}};
    Scene scene;
    if (!scene.create(itemPixels(1), count)) { out["error"] = scene.error; return out; }
    QJsonArray tfList;
    for (int i = 0; i < count; ++i) {
        scene.items[size_t(i)]->setTimeframeMinutes(tfs[i]);
        scene.items[size_t(i)]->setPrepMode(mode);
        tfList.append(tfs[i]);
    }
    const auto start = Clock::now();
    for (auto *item : scene.items) item->loadReal(24, layer); // Auto tick, default views
    const auto w = waitFor(scene, [&] {
        return std::all_of(scene.items.begin(), scene.items.end(), [](LabItem *i) { return i->settled(); });
    }, start);
    out["timeframes"] = tfList;
    out["settle"] = w.json();
    double gpu = 0, prepCpu = 0;
    QJsonArray perChart;
    for (auto *item : scene.items) {
        const auto m = item->metrics();
        gpu += m.value("gpuBytes").toDouble();
        prepCpu += m.value("prepCpuBytes").toDouble();
        perChart.append(QJsonObject{{"tf_minutes", m.value("timeframeMinutes").toInt()},
                                    {"gpu_bytes", m.value("gpuBytes").toDouble()},
                                    {"prep_cpu_bytes", m.value("prepCpuBytes").toDouble()},
                                    {"tick", m.value("tick").toDouble()}});
    }
    const auto store = chunkStore().stats();
    const auto inter = intermediateStats();
    out["per_chart"] = perChart;
    out["gpu_bytes"] = gpu;
    out["prep_cpu_bytes"] = prepCpu;
    out["chunk_store_bytes"] = double(store.bytes);
    out["chunk_entries"] = double(store.entries);
    out["chunk_loads"] = double(store.loads);
    out["chunk_shared_loads"] = double(store.sharedLoads);
    out["chunk_hits"] = double(store.hits);
    out["intermediate_bytes"] = double(inter.bytes);
    out["footprint_bytes"] = double(processFootprintBytes());
    out["redundant_decodes"] = double(store.loads) - double(store.entries) - double(store.evictions);
    drainWorkers();
    return out;
}

QString fmt(double v, int precision = 0) { return QString::number(v, 'f', precision); }
QString cell(const QJsonObject &o, const char *key, int precision = 0) {
    return o.contains(key) ? fmt(o.value(key).toDouble(), precision) : QStringLiteral("-");
}
} // namespace

int runB1Bench(const QString &jsonPath, bool quick) {
    if (!metalDeviceAvailable()) {
        std::cout << "{\"error\":\"No MTLDevice (Metal unavailable)\"}" << std::endl;
        return 2;
    }
    std::vector<Case> cases;
    const std::vector<PrepMode> modes{PrepMode::Viewport, PrepMode::WholeChunkGpu, PrepMode::WholeChunkCpu};
    for (const int dpr : quick ? std::vector<int>{1} : std::vector<int>{1, 2})
        for (const QString &layer : {QStringLiteral("near"), QStringLiteral("deep")})
            for (const int tf : quick ? std::vector<int>{1, 60} : std::vector<int>{1, 5, 60})
                for (const double tick : layer == "near" ? std::vector<double>{1, 10, 100} : std::vector<double>{10, 100})
                    for (const auto mode : modes) cases.push_back({mode, dpr, layer, tf, tick});
    QJsonArray sessions;
    const auto started = Clock::now();
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto &c = cases[i];
        std::cerr << "[" << i + 1 << "/" << cases.size() << "] " << prepModeName(c.mode).toStdString() << " " << c.dpr
                  << "x " << c.layer.toStdString() << " " << c.tfMinutes << "m $" << c.tick << std::flush;
        const auto s = runSession(c);
        std::cerr << (s.contains("error") ? " ERROR " + s.value("error").toString().toStdString() : "")
                  << " (" << int(since(started) / 1000) << " s)\n";
        sessions.append(s);
    }
    QJsonArray multi;
    for (const auto mode : modes)
        for (const QString &layer : {QStringLiteral("near"), QStringLiteral("deep")})
            for (const int count : {1, 2, 4}) {
                std::cerr << "multi-chart " << prepModeName(mode).toStdString() << " " << layer.toStdString() << " x"
                          << count << "\n";
                multi.append(runMultiChart(mode, layer, count));
            }
    const QJsonObject result{{"sessions", sessions}, {"multi_chart", multi}, {"frame_pacing_hz", 60},
                             {"upload_budget_bytes", double(LabItem::kDefaultUploadBudgetBytes)},
                             {"tile_budget_bytes", double(LabItem::kDefaultTileBudgetBytes)},
                             {"tile_columns", double(heatmap::tiles::kTileColumns)},
                             {"elapsed_s", since(started) / 1000}};
    if (!jsonPath.isEmpty()) {
        QFile file(jsonPath);
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    }
    // Comparison table (ms at 60 Hz pacing; latency = input to the first fully drawn frame).
    std::printf("%-15s %2s %-4s %4s %5s | %8s %8s | %7s %8s | %7s %8s | %8s %8s | %7s %7s %7s | %6s\n", "mode", "px",
                "lay", "tf", "tick", "ttfvCold", "ttfvWarm", "vPan95", "vPanBig", "hPan95", "hPanBig", "tickChg",
                "revise", "gpuMB", "peakMB", "cpuMB", "hit%");
    for (const auto &v : sessions) {
        const auto s = v.toObject();
        const auto mem = s.value("memory").toObject();
        const double cpu = mem.value("prep_cpu_bytes").toDouble() + mem.value("intermediate_bytes").toDouble();
        std::printf("%-15s %2s %-4s %4s %5s | %8s %8s | %7s %8s | %7s %8s | %8s %8s | %7s %7s %7s | %6s\n",
                    qPrintable(s.value("mode").toString()), qPrintable(QString::number(s.value("dpr").toInt()) + "x"),
                    qPrintable(s.value("layer").toString()),
                    qPrintable(QString::number(s.value("tf_minutes").toInt()) + "m"),
                    qPrintable("$" + QString::number(s.value("tick").toDouble())),
                    qPrintable(cell(s.value("ttfv_cold").toObject(), "ms")),
                    qPrintable(cell(s.value("ttfv_warm").toObject(), "ms")),
                    qPrintable(cell(s.value("vpan_small").toObject(), "ms_p95")),
                    qPrintable(cell(s.value("vpan_large").toObject(), "ms")),
                    qPrintable(cell(s.value("hpan_small").toObject(), "ms_p95")),
                    qPrintable(cell(s.value("hpan_large").toObject(), "ms")),
                    qPrintable(cell(s.value("zoom_tick_change").toObject(), "ms")),
                    qPrintable(cell(s.value("revise").toObject(), "ms")),
                    qPrintable(fmt(mib(mem.value("gpu_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(mem.value("gpu_bytes_peak").toDouble()), 1)), qPrintable(fmt(mib(cpu), 1)),
                    qPrintable(fmt(100 * s.value("cache").toObject().value("chunk_hit_rate").toDouble(), 0)));
    }
    std::printf("\n%-15s %-4s %6s | %8s %8s %8s %8s %6s %6s | %7s %8s\n", "mode", "lay", "charts", "gpuMB", "prepMB",
                "chunkMB", "interMB", "loads", "shared", "redund", "settleMs");
    for (const auto &v : multi) {
        const auto s = v.toObject();
        std::printf("%-15s %-4s %6d | %8s %8s %8s %8s %6s %6s | %7s %8s\n", qPrintable(s.value("mode").toString()),
                    qPrintable(s.value("layer").toString()), s.value("charts").toInt(),
                    qPrintable(fmt(mib(s.value("gpu_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(s.value("prep_cpu_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(s.value("chunk_store_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(s.value("intermediate_bytes").toDouble()), 1)),
                    qPrintable(cell(s, "chunk_loads")), qPrintable(cell(s, "chunk_shared_loads")),
                    qPrintable(cell(s, "redundant_decodes")), qPrintable(cell(s.value("settle").toObject(), "ms")));
    }
    std::fflush(stdout);
    return 0;
}
} // namespace lab
