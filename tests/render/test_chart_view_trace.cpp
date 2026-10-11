#include "UnifiedGridRenderer.h"
#include "datasources/CandleSeriesBuffer.hpp"
#include "lab/LabData.hpp"
#include "render/GridViewState.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "SyntheticHmc2Fixture.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace synthetic_hmc2;

recording::Hmc2Record tinyMinute(const std::string& layer, int64_t i) {
    auto r = syntheticMinute(layer, i);
    r.header.symbol = "TINY-USD";
    r.header.priceScale = 1e10;
    r.midOpen = r.midClose = r.midMin = r.midMax = r.midOpen * 100 / 1e10;
    return r;
}

void configureFixture(QTemporaryDir& dir) {
    writeRecording(dir, 4 * 60);
    lab::LabData::setLocalSymbolsForTest({"BTC-USD", "TINY-USD"});
    {
        recording::Hmc2Store writer(dir.path().toStdString());
        for (int64_t i = 0; i < 4 * 60; ++i)
            for (const char* layer : {"deep", "near"}) writer.append(tinyMinute(layer, i));
    }
    lab::LabData::configure(dir.path().toStdString(), epoch + 4 * kHourMs);
}

class CpuChart : public UnifiedGridRenderer {
public:
    using UnifiedGridRenderer::updatePolish;
};

void appendDouble(std::string& out, const char* key, double value) {
    char text[96];
    std::snprintf(text, sizeof(text), " %s=%a", key, value);
    out += text;
}

template<class T> void appendInt(std::string& out, const char* key, T value) {
    out += " "; out += key; out += "="; out += std::to_string(value);
}

void appendCamera(std::string& out, const char* prefix, const chart_raster::RasterCamera& c) {
    const std::string p(prefix);
    appendInt(out, (p + ".valid").c_str(), c.valid);
    appendInt(out, (p + ".rowPx").c_str(), c.rowPx);
    appendInt(out, (p + ".colPx").c_str(), c.colPx);
    appendDouble(out, (p + ".dpr").c_str(), c.dpr);
    appendInt(out, (p + ".widthDev").c_str(), c.widthDev);
    appendInt(out, (p + ".heightDev").c_str(), c.heightDev);
    appendInt(out, (p + ".topRowIndex").c_str(), c.topRowIndex);
    appendInt(out, (p + ".leftColIndex").c_str(), c.leftColIndex);
    appendDouble(out, (p + ".tfMs").c_str(), c.tfMs);
    appendDouble(out, (p + ".tick").c_str(), c.tick);
    appendDouble(out, (p + ".drawnStartMs").c_str(), c.drawnStartMs);
    appendDouble(out, (p + ".drawnEndMs").c_str(), c.drawnEndMs);
    appendDouble(out, (p + ".drawnMinPrice").c_str(), c.drawnMinPrice);
    appendDouble(out, (p + ".drawnMaxPrice").c_str(), c.drawnMaxPrice);
    appendInt(out, (p + ".free").c_str(), c.free);
    appendDouble(out, (p + ".rowPxF").c_str(), c.rowPxF);
    appendDouble(out, (p + ".colPxF").c_str(), c.colPxF);
    appendDouble(out, (p + ".topF").c_str(), c.topF);
    appendDouble(out, (p + ".leftF").c_str(), c.leftF);
}

class Trace {
    CpuChart chart;
    CandleSeriesBuffer candles;
    qint64 clock = 0;
    const qint64 centre = epoch + 2 * kHourMs + 15 * minute;
    std::string output;
    std::string symbol = "BTC-USD";
public:
    Trace() {
        chart.setSize({640, 320});
        chart.setHeatmapService(&lab::LabData::instance().service());
        chart.setActiveSymbol("BTC-USD");
        chart.setTimeframe(int(minute));
        heatmap::HeatmapChartSettings settings;
        settings.palettePreset = "Fire";
        settings.sensitivityMin = 0.001;
        settings.sensitivityMax = 1;
        settings.showLabels = false;
        chart.setHeatmapChartSettings(settings);
        chart.setZoomClockForTest([this] { return clock; });
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        for (int i = -60; i < 60; ++i) {
            const double radius = 8 + 7 * std::max(std::abs(i), std::abs(i + 1));
            const qint64 t = centre + i * minute;
            bars.push_back({t, t + minute, 100150, 100150 + radius, 100150 - radius, 100150, 1, true, 0, false});
        }
        candles.applyHistory("BTC-USD", 60, bars);
        chart.setCandleBuffer(&candles);
    }
    ~Trace() {
        chart.setCandleBuffer(nullptr);
        chart.setHeatmapService(nullptr);
    }
    void frame() {
        auto* v = chart.getViewState();
        chart.gpuHeatmapLayer()->chooseTickForView({double(v->getVisibleTimeStart()), double(v->getVisibleTimeEnd()),
                                                    v->getMinPrice(), v->getMaxPrice()});
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        chart.updatePolish();
    }
    void ready(bool needSource = true) {
        QElapsedTimer elapsed;
        elapsed.start();
        do {
            frame();
            const auto snapshot = chart.gpuHeatmapLayer()->snapshot();
            const bool source = snapshot && snapshot->symbol == symbol &&
                                !snapshot->resolution.columns.empty() && !snapshot->spans.empty() &&
                                std::all_of(snapshot->spans.begin(), snapshot->spans.end(),
                                            [](const auto& s) { return s.complete; });
            if ((!needSource || source) && (!needSource || chart.heatmapTickSize() > 0)) return;
        } while (elapsed.elapsed() < 10000);
        throw std::runtime_error("source/tick did not become ready");
    }
    void step(const char* name, bool needSource = true) {
        ready(needSource);
        auto* v = chart.getViewState();
        std::string line(name);
        appendInt(line, "start", v->getVisibleTimeStart());
        appendInt(line, "end", v->getVisibleTimeEnd());
        appendDouble(line, "min", v->getMinPrice());
        appendDouble(line, "max", v->getMaxPrice());
        appendInt(line, "autoScroll", chart.autoScrollEnabled());
        appendInt(line, "autoPriceScale", chart.autoPriceScale());
        const auto anchor = v->rasterAnchor();
        appendDouble(line, "rasterAnchor.x", anchor.fracX);
        appendDouble(line, "rasterAnchor.y", anchor.fracY);
        appendInt(line, "viewportVersion", v->getViewportVersion());
        appendInt(line, "placementVersion", v->placementVersion());
        appendCamera(line, "raster", chart.rasterCameraNow(false));
        appendCamera(line, "rest", chart.restCameraNow(false));
        appendInt(line, "timeGliding", chart.timeGliding());
        appendInt(line, "priceGliding", chart.priceGliding());
        appendInt(line, "zoomGesturing", chart.zoomGesturing());
        appendInt(line, "glideRebases", chart.glideRebasesForTest());
        const auto tick = chart.gpuHeatmapLayer()->autoPriceTickUnits();
        appendInt(line, "autoPriceTickUnits", tick.value_or(-1));
        output += line + "\n";
    }
    void advance(qint64 delta, const char* name) { clock += delta; step(name); }
    void run() {
        step("cold-bootstrap", false);
        chart.setViewport(centre - 20 * minute, centre + 20 * minute, 99800, 100500);
        ready();
        chart.setViewport(centre - 2 * minute, centre + 2 * minute, 100120, 100180);
        chart.setAutoPriceScale(true);
        step("initial");
        const double points[][2] = {{120, 70}, {320, 160}, {520, 250}};
        for (int i = 0; i < 3; ++i) {
            chart.zoomClicks(1, points[i][0], points[i][1]);
            step("wheel-in-0"); advance(65, "wheel-in-65"); advance(65, "wheel-in-130"); advance(170, "wheel-in-300");
            chart.zoomClicks(-1, points[i][0], points[i][1]);
            step("wheel-out-0"); advance(65, "wheel-out-65"); advance(65, "wheel-out-130"); advance(170, "wheel-out-300");
        }
        chart.zoomContinuous(1.13, 290, 140);
        step("trackpad");
        chart.endZoomGesture(); advance(300, "trackpad-settle");
        chart.zoomTimeDrag(34, 230); step("time-drag");
        chart.endZoomGesture(); advance(300, "time-drag-settle");
        chart.zoomPriceDrag(-27, 130); step("price-drag");
        chart.endZoomGesture(); advance(300, "price-drag-settle");
        chart.zoomClicks(1, 240, 130); advance(65, "pan-glide");
        chart.beginPanAt(250, 150); chart.updatePanAt(279, 160); step("pan-active");
        chart.endPanAt(); step("pan-release"); advance(300, "pan-settle");
        chart.enableAutoScroll(true); chart.followLiveForTest(centre + 4 * minute); step("follow-1");
        chart.followLiveForTest(centre + 9 * minute); step("follow-2");
        chart.setTimeframe(int(15 * minute)); step("tf-15m-follow");
        chart.setTimeframe(int(minute)); step("tf-1m-follow");
        chart.enableAutoScroll(false); chart.setViewport(centre - 8 * minute, centre + 8 * minute, 100100, 100200);
        chart.setTimeframe(int(15 * minute)); step("tf-15m-history");
        chart.setTimeframe(int(minute)); step("tf-1m-history");
        chart.setAutoPriceScale(false); chart.setActiveSymbol("TINY-USD"); symbol = "TINY-USD"; step("symbol-tiny");
        chart.setActiveSymbol("BTC-USD"); symbol = "BTC-USD"; step("symbol-btc");
        chart.fitView(true, false); step("fit-time");
        chart.fitView(false, true); step("fit-price");
        chart.fitView(true, true); step("fit-both");
        chart.resetView(); step("reset");
        UnifiedGridRenderer::ViewportRequest request;
        request.startMs = centre - 3 * minute; request.endMs = centre + 3 * minute;
        chart.applyViewportRequest(request); step("request-time");
        request = {}; request.priceMin = 100000; request.priceMax = 100300;
        chart.applyViewportRequest(request); step("request-price");
        request = {}; request.autoScale = true; request.followLive = true;
        chart.applyViewportRequest(request); step("request-flags");
        request = {}; request.fit = "default";
        chart.applyViewportRequest(request); step("request-default");
    }
    const std::string& text() const { return output; }
};
} // namespace

int main(int argc, char** argv) {
    bool bless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bless") bless = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    try {
        QTemporaryDir dir;
        if (!dir.isValid()) throw std::runtime_error("temporary fixture directory failed");
        configureFixture(dir);
        Trace trace;
        trace.run();
        QFile golden(QStringLiteral(SENTINEL_SOURCE_DIR "/tests/render/golden/view_trace.txt"));
        if (bless) {
            if (!golden.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                golden.write(trace.text().data(), qsizetype(trace.text().size())) != qsizetype(trace.text().size()))
                throw std::runtime_error("cannot write golden");
            std::puts("ChartViewTrace: blessed");
            return 0;
        }
        if (!golden.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read golden");
        const QByteArray expected = golden.readAll();
        if (expected != QByteArray(trace.text().data(), qsizetype(trace.text().size()))) {
            std::fprintf(stderr, "ChartViewTrace: golden differs (expected %lld bytes, actual %zu bytes)\n",
                         static_cast<long long>(expected.size()), trace.text().size());
            return 1;
        }
        std::puts("ChartViewTrace: PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ChartViewTrace: %s\n", e.what());
        return 1;
    }
}
