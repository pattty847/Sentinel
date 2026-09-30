#include "S5Bench.hpp"
#include "LabData.hpp"
#include "LabItem.hpp"
#include "LabSources.hpp"
#include "OffscreenQuick.hpp"
#include "RhiBackend.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
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
QJsonObject series(const std::vector<Wait> &waits) {
    std::vector<double> ms, frames;
    int failed = 0;
    for (const auto &w : waits) {
        ms.push_back(w.ms);
        frames.push_back(w.frames);
        failed += !w.ok;
    }
    return {{"steps", int(waits.size())}, {"failed", failed}, {"ms_p50", percentile(ms, 0.5)},
            {"ms_p95", percentile(ms, 0.95)}, {"ms_max", percentile(ms, 1)}, {"frames_p50", percentile(frames, 0.5)},
            {"frames_max", percentile(frames, 1)}};
}

struct Case {
    int dpr;
    int tfMinutes;
    double tick;
};
QSize itemPixels(int dpr) { return dpr == 2 ? QSize(3000, 1760) : QSize(1500, 880); }
double metric(const LabItem *item, const char *key) { return item->metrics().value(key).toDouble(); }

struct Range {
    int64_t oldestMs = 0, endMs = 0;
};
Range recordedRange() {
    Range r{INT64_MAX, 0};
    if (const auto a = LabData::instance().availability())
        for (const auto &source : a->sources)
            for (const auto &level : source.levels) {
                r.oldestMs = std::min(r.oldestMs, level.oldestMs);
                r.endMs = std::max(r.endMs, level.committedThroughMs);
            }
    return r;
}
bool waitForAvailability() {
    const auto start = Clock::now();
    while (since(start) < 30'000) {
        if (LabData::instance().availability()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return false;
}

// The session's view: newest data at the right edge, 3 physical px per column
// (all the data when it is shorter), rows of the fixed tick 3 physical px tall.
heatmap::gpu::ViewWindow sessionView(const Case &c, const Range &range, double price) {
    const QSize px = itemPixels(c.dpr);
    const int64_t tf = int64_t(c.tfMinutes) * heatmap::kMinuteMs;
    const double end = double(recording::floorDiv(range.endMs + tf - 1, tf) * tf);
    const double span = std::min(double(px.width() / 3) * double(tf), end - double(range.oldestMs));
    const double priceSpan = double(px.height()) / 3.0 * c.tick;
    return {end - span, end, price - priceSpan / 2, price + priceSpan / 2};
}

QJsonObject memoryOf(const LabItem *item) {
    const auto m = item->metrics();
    return {{"gpu_bytes", m.value("gpuBytes").toDouble()}, {"resident_bytes", m.value("residentBytes").toDouble()},
            {"source_bytes", m.value("sourceBytes").toDouble()}, {"bin_bytes", m.value("binBytes").toDouble()},
            {"resident_sources", m.value("residentSources").toDouble()},
            {"chunk_store_bytes", m.value("chunkBytes").toDouble()},
            {"span_live_bytes", m.value("spanLiveBytes").toDouble()},
            {"cpu_committed_bytes", m.value("cpuCommittedBytes").toDouble()},
            {"footprint_bytes", m.value("footprintBytes").toDouble()}};
}

QJsonObject runSession(const Case &c, double price) {
    QJsonObject out{{"dpr", c.dpr}, {"tf_minutes", c.tfMinutes}, {"tick", c.tick}};
    const QSize px = itemPixels(c.dpr);
    auto makeScene = [&](Scene &scene, const heatmap::gpu::ViewWindow &view) {
        if (!scene.create(px, 1)) return false;
        auto *item = scene.items[0];
        item->setTimeframeMinutes(c.tfMinutes);
        item->setManualTick(c.tick);
        item->setInitialView(view);
        return true;
    };
    // 1. Cold: a fresh data path (no decoded chunk, no span source anywhere).
    LabData::instance().clearCaches();
    if (!waitForAvailability()) { out["error"] = QStringLiteral("no availability"); return out; }
    const auto range = recordedRange();
    const auto view = sessionView(c, range, price);
    out["view_minutes"] = (view.timeHiMs - view.timeLoMs) / 60'000.0;
    out["view_price_span"] = view.priceHi - view.priceLo;
    {
        Scene scene;
        if (!makeScene(scene, view)) { out["error"] = scene.error; return out; }
        auto *item = scene.items[0];
        const auto start = Clock::now();
        item->loadReal(24);
        const auto w = waitFor(scene, [&] { return item->settled(); }, start);
        out["ttfv_cold"] = w.json();
        out["cold_chunk_loads"] = metric(item, "chunkLoads");
    }
    // 2. Warm: another chart decoded the chunks and built the spans (the span
    // cache keeps unclaimed builds within its tier).
    Scene scene;
    if (!makeScene(scene, view)) { out["error"] = scene.error; return out; }
    auto *item = scene.items[0];
    auto settled = [&] { return item->settled(); };
    double peakGpu = 0;
    auto trackPeak = [&] { peakGpu = std::max(peakGpu, metric(item, "gpuBytes")); };
    {
        const auto start = Clock::now();
        item->loadReal(24);
        const auto w = waitFor(scene, settled, start);
        out["ttfv_warm"] = w.json();
        out["initial_memory"] = memoryOf(item);
        if (!w.ok) { out["error"] = QStringLiteral("warm start did not settle: ") + item->status(); return out; }
    }
    trackPeak();
    auto step = [&](const std::function<void()> &input) {
        const auto start = Clock::now();
        input();
        const auto w = waitFor(scene, settled, start);
        trackPeak();
        if (!w.ok) // what the chart was waiting for
            std::cerr << "\n  did not settle: "
                      << QJsonDocument(QJsonObject::fromVariantMap(item->metrics())).toJson(QJsonDocument::Compact).toStdString()
                      << "\n";
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
        for (int i = 0; i < 20; ++i) waits.push_back(step([&] { item->pan(0, 0.05 * height); }));
        out["vpan_small"] = series(waits);
    }
    // 4. One large vertical pan (3 view heights): leaves the binned rows.
    out["vpan_large"] = step([&] { item->pan(0, 3 * height); }).json();
    reset();
    // 5. Small horizontal pans (5 % of the width, back in time).
    {
        std::vector<Wait> waits;
        for (int i = 0; i < 20; ++i) waits.push_back(step([&] { item->pan(0.05 * width, 0); }));
        out["hpan_small"] = series(waits);
    }
    // 6. One large horizontal pan 3 views back (or as far as the recording goes).
    {
        const auto v = item->view();
        const double span = v.timeHiMs - v.timeLoMs;
        const double shift = std::min(3 * span, v.timeLoMs - double(range.oldestMs));
        if (shift >= span) {
            const double loads = metric(item, "chunkLoads");
            auto j = step([&] { item->pan(shift / span * width, 0); }).json();
            j["views"] = shift / span;
            j["chunk_loads"] = metric(item, "chunkLoads") - loads;
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
            const auto w = step([&] { item->zoom(-1, true, 0.5, 0.5); });
            const double tick = metric(item, "tick");
            if (tick != from) {
                j = w.json();
                j["from_tick"] = from;
                j["to_tick"] = tick;
                j["zoom_steps"] = i + 1;
                break;
            }
        }
        out["zoom_tick_change"] = j;
        item->setManualTick(c.tick);
    }
    reset();
    // 8. A revised newest chunk (a new generation of each source's newest chunk):
    // until the visible spans show the new builds, fully drawn.
    {
        const auto start = Clock::now();
        const auto revised = LabData::instance().reviseNewestChunks();
        // Done once a visible span shows a revised chunk's new generation, fully drawn.
        auto uses = [&] {
            const auto set = item->snapshot();
            if (!set) return false;
            for (const auto &span : set->spans)
                if (span.rank.tier == heatmap::SpanTier::Visible)
                    for (const auto &source : span.sources)
                        if (source.build)
                            for (const auto &g : source.build->key.generations)
                                for (const auto &[key, generation] : revised)
                                    if (g.source == key.source && g.startMs == key.startMs && g.generation == generation)
                                        return true;
            return false;
        };
        auto j = waitFor(scene, [&] { return uses() && item->settled(); }, start, 20'000).json();
        j["revised_chunks"] = int(revised.size());
        j["span_builds"] = metric(item, "spanBuilds");
        out["revise"] = j;
    }
    trackPeak();
    // 9. Recent timeframe: to another timeframe (settled), then back.
    {
        const int other = c.tfMinutes == 5 ? 1 : 5;
        const auto away = step([&] { item->setTimeframeMinutes(other); });
        const double requests = metric(item, "fetchedChunks"), builds = metric(item, "spanBuilds");
        auto j = step([&] { item->setTimeframeMinutes(c.tfMinutes); }).json();
        j["away_ms"] = away.ms;
        j["away_tf_minutes"] = other;
        j["chunk_requests"] = metric(item, "fetchedChunks") - requests;
        j["span_builds"] = metric(item, "spanBuilds") - builds;
        out["tf_switch_back"] = j;
    }
    trackPeak();
    // Steady state: half a second more, so tier changes (fallback -> recent-tf)
    // reach the node's cap.
    for (int i = 0; i < 30; ++i) {
        scene.frame();
        trackPeak();
    }
    auto memory = memoryOf(item);
    memory["gpu_bytes_peak"] = peakGpu;
    out["memory"] = memory;
    out["errors"] = metric(item, "errors");
    out["missing_draws"] = metric(item, "missingDraws");
    return out;
}

QJsonObject runMultiChart(int count) {
    static const int tfs[] = {1, 5, 60, 15};
    LabData::instance().clearCaches();
    waitForAvailability();
    QJsonObject out{{"charts", count}};
    Scene scene;
    if (!scene.create(itemPixels(1), count)) { out["error"] = scene.error; return out; }
    QJsonArray tfList;
    for (int i = 0; i < count; ++i) {
        scene.items[size_t(i)]->setTimeframeMinutes(tfs[i]);
        tfList.append(tfs[i]);
    }
    const auto start = Clock::now();
    for (auto *item : scene.items) item->loadReal(24); // Auto tick, default views
    const auto w = waitFor(scene, [&] {
        return std::all_of(scene.items.begin(), scene.items.end(), [](LabItem *i) { return i->settled(); });
    }, start);
    out["timeframes"] = tfList;
    out["settle"] = w.json();
    double gpu = 0;
    QJsonArray perChart;
    for (auto *item : scene.items) {
        const auto m = item->metrics();
        gpu += m.value("gpuBytes").toDouble();
        perChart.append(QJsonObject{{"tf_minutes", m.value("timeframeMinutes").toInt()},
                                    {"gpu_bytes", m.value("gpuBytes").toDouble()}, {"tick", m.value("tick").toDouble()},
                                    {"refused_spans", m.value("refusedSpans").toInt()}});
    }
    LabData::instance().refreshStats();
    const auto s = LabData::instance().stats();
    out["per_chart"] = perChart;
    out["gpu_bytes"] = gpu;
    out["chunk_store_bytes"] = double(s.store.bytes);
    out["chunk_entries"] = double(s.store.entries);
    out["chunk_loads"] = double(s.store.loads);
    out["chunk_evictions"] = double(s.store.evictions);
    out["fetched_chunks"] = double(s.fetcher.requestedChunks);
    out["span_builds"] = double(s.cache.builds);
    out["span_shared"] = double(s.cache.sharedBuilds + s.cache.hits);
    out["cpu_committed_bytes"] = double(s.committedCpuBytes);
    out["footprint_bytes"] = double(processFootprintBytes());
    out["redundant_decodes"] = double(s.store.loads) - double(s.store.entries) - double(s.store.evictions);
    return out;
}

QString fmt(double v, int precision = 0) { return QString::number(v, 'f', precision); }
QString cell(const QJsonObject &o, const char *key, int precision = 0) {
    return o.contains(key) ? fmt(o.value(key).toDouble(), precision) : QStringLiteral("-");
}
void printError(const QString &why) {
    std::cout << QJsonDocument(QJsonObject{{"error", why}}).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}
} // namespace

int runS5Bench(const QString &jsonPath, bool quick) {
    // The JSON is opened WriteOnly: never inside the recording root. Checked
    // before the run and again before the write.
    if (QString why; !jsonPath.isEmpty() && !labOutputAllowed(jsonPath, &why)) { printError(why); return 2; }
    if (const QString why = gpuUnavailableReason(); !why.isEmpty()) { printError(why); return 2; }
    if (!waitForAvailability()) { printError(QStringLiteral("no recording availability")); return 2; }
    const double price = LabData::instance().recentMidPrice();
    if (!(price > 0)) { printError(QStringLiteral("no recent price")); return 2; }
    std::vector<Case> cases;
    for (const int dpr : quick ? std::vector<int>{1} : std::vector<int>{1, 2})
        for (const int tf : quick ? std::vector<int>{1, 60} : std::vector<int>{1, 5, 60})
            for (const double tick : {1.0, 10.0, 100.0}) cases.push_back({dpr, tf, tick});
    QJsonArray sessions;
    const auto started = Clock::now();
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto &c = cases[i];
        std::cerr << "[" << i + 1 << "/" << cases.size() << "] " << c.dpr << "x " << c.tfMinutes << "m $" << c.tick
                  << std::flush;
        const auto s = runSession(c, price);
        std::cerr << (s.contains("error") ? " ERROR " + s.value("error").toString().toStdString() : "") << " ("
                  << int(since(started) / 1000) << " s)\n";
        sessions.append(s);
    }
    QJsonArray multi;
    for (const int count : {1, 2, 4}) {
        std::cerr << "multi-chart x" << count << "\n";
        multi.append(runMultiChart(count));
    }
    const QJsonObject result{{"sessions", sessions}, {"multi_chart", multi}, {"frame_pacing_hz", 60},
                             {"upload_budget_bytes", double(LabItem::kDefaultUploadBudgetBytes)},
                             {"gpu_cap_bytes", double(heatmap::HeatmapBudgets{}.gpuPerChart)},
                             {"elapsed_s", since(started) / 1000}, {"price", price},
                             {"pinned_end_ms", double(LabData::pinnedEndMs())}};
    if (!jsonPath.isEmpty()) {
        if (QString why; !labOutputAllowed(jsonPath, &why)) { printError(why); return 2; }
        QFile file(jsonPath);
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    }
    // ms at 60 Hz pacing; latency = input to the first frame fully drawn from
    // complete content of the current timeframe and tick.
    std::printf("%2s %4s %5s | %8s %8s | %7s %8s | %7s %8s | %8s %8s %7s | %7s %7s | %s\n", "px", "tf", "tick",
                "ttfvCold", "ttfvWarm", "vPan95", "vPanBig", "hPan95", "hPanBig", "tickChg", "revise", "tfBack",
                "gpuMB", "peakMB", "tfBack req/builds");
    for (const auto &v : sessions) {
        const auto s = v.toObject();
        const auto mem = s.value("memory").toObject();
        const auto back = s.value("tf_switch_back").toObject();
        std::printf("%2s %4s %5s | %8s %8s | %7s %8s | %7s %8s | %8s %8s %7s | %7s %7s | %s/%s%s\n",
                    qPrintable(QString::number(s.value("dpr").toInt()) + "x"),
                    qPrintable(QString::number(s.value("tf_minutes").toInt()) + "m"),
                    qPrintable("$" + QString::number(s.value("tick").toDouble())),
                    qPrintable(cell(s.value("ttfv_cold").toObject(), "ms")),
                    qPrintable(cell(s.value("ttfv_warm").toObject(), "ms")),
                    qPrintable(cell(s.value("vpan_small").toObject(), "ms_p95")),
                    qPrintable(cell(s.value("vpan_large").toObject(), "ms")),
                    qPrintable(cell(s.value("hpan_small").toObject(), "ms_p95")),
                    qPrintable(cell(s.value("hpan_large").toObject(), "ms")),
                    qPrintable(cell(s.value("zoom_tick_change").toObject(), "ms")),
                    qPrintable(cell(s.value("revise").toObject(), "ms")), qPrintable(cell(back, "ms")),
                    qPrintable(fmt(mib(mem.value("gpu_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(mem.value("gpu_bytes_peak").toDouble()), 1)),
                    qPrintable(cell(back, "chunk_requests")), qPrintable(cell(back, "span_builds")),
                    s.contains("error") ? qPrintable(" ERROR " + s.value("error").toString()) : "");
    }
    std::printf("\n%6s | %8s %8s %6s %6s %7s | %7s %8s\n", "charts", "gpuMB", "chunkMB", "loads", "builds", "shared",
                "redund", "settleMs");
    for (const auto &v : multi) {
        const auto s = v.toObject();
        std::printf("%6d | %8s %8s %6s %6s %7s | %7s %8s\n", s.value("charts").toInt(),
                    qPrintable(fmt(mib(s.value("gpu_bytes").toDouble()), 1)),
                    qPrintable(fmt(mib(s.value("chunk_store_bytes").toDouble()), 1)), qPrintable(cell(s, "chunk_loads")),
                    qPrintable(cell(s, "span_builds")), qPrintable(cell(s, "span_shared")),
                    qPrintable(cell(s, "redundant_decodes")), qPrintable(cell(s.value("settle").toObject(), "ms")));
    }
    std::fflush(stdout);
    return 0;
}
} // namespace lab
