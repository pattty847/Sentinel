// Slice S0b: Metal frame-hash golden of the main chart.
//
// Twelve scenes, each on a fresh 640x320 lab::OffscreenQuick scene (UGR, a C++-built
// CandlestickOverlayItem and a TradeBubbleOverlayItem), hashed only once the frame has
// settled: SHA-256 of the RGBA readback plus an explicit, named, hex-float serialization
// of the published mapping (never raw struct bytes). The manifest
// (golden/frames.json) is keyed by backend + device. On the owner's Mac M4 with Metal a
// missing baseline or scene FAILS; other machines skip loudly (exit 77).
//   --bless       write (merge) this machine's key into the manifest
// A mismatch dumps PNGs to SENTINEL_GOLDEN_OUT_DIR (<build>/golden-out).
#include "UnifiedGridRenderer.h"
#include "SyntheticHmc2Fixture.hpp"
#include "datasources/CandleSeriesBuffer.hpp"
#include "lab/LabData.hpp"
#include "lab/OffscreenQuick.hpp"
#include "lab/RhiBackend.hpp"
#include "marketdata/model/TradeData.h"
#include "render/CandlestickOverlayItem.hpp"
#include "render/DataProcessor.hpp"
#include "render/TradeBubbleOverlayItem.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <rhi/qrhi.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace synthetic_hmc2;

constexpr const char* kRequiredKey = "Metal:Apple M4"; // the owner's Mac: baselines are mandatory there
const int64_t viewLo = epoch + 2 * kHourMs, viewHi = epoch + 2 * kHourMs + 40 * minute;

QString outDir();
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };

void appendDouble(std::string& out, const char* key, double value) {
    char text[96];
    std::snprintf(text, sizeof(text), " %s=%a", key, value);
    out += text;
}
template <class T> void appendInt(std::string& out, const char* key, T value) {
    out += " "; out += key; out += "="; out += std::to_string(value);
}
void appendRect(std::string& out, const char* key, const QRectF& r) {
    const std::string p(key);
    appendDouble(out, (p + ".x").c_str(), r.x()); appendDouble(out, (p + ".y").c_str(), r.y());
    appendDouble(out, (p + ".w").c_str(), r.width()); appendDouble(out, (p + ".h").c_str(), r.height());
}

// Every field is written by name in a fixed order. No wall-clock fields (presentationTimeMs,
// nowEventTimeMs, boundaries): they move with the clock, not with the picture.
std::string serializeFrame(const MappingFrameContext& f) {
    std::string s;
    appendRect(s, "surface", f.surfaceBounds);
    appendDouble(s, "dpr", f.surfaceDpr);
    appendInt(s, "activeTimeframeMs", f.activeTimeframeMs);
    appendInt(s, "viewportValid", f.viewportValid);
    appendInt(s, "viewportTimeStart", f.viewportTimeStart);
    appendInt(s, "viewportTimeEnd", f.viewportTimeEnd);
    appendDouble(s, "viewportMinPrice", f.viewportMinPrice);
    appendDouble(s, "viewportMaxPrice", f.viewportMaxPrice);
    appendDouble(s, "panX", f.viewportPanVisualOffset.x());
    appendDouble(s, "panY", f.viewportPanVisualOffset.y());
    appendInt(s, "dragging", f.viewportDragging);
    appendInt(s, "autoScroll", f.viewportAutoScrollEnabled);
    const auto& m = f.mapping;
    appendDouble(s, "m.viewStartMs", m.viewStartMs); appendDouble(s, "m.viewEndMs", m.viewEndMs);
    appendDouble(s, "m.viewMinPrice", m.viewMinPrice); appendDouble(s, "m.viewMaxPrice", m.viewMaxPrice);
    appendDouble(s, "m.dataStartMs", m.dataStartMs); appendDouble(s, "m.dataEndMs", m.dataEndMs);
    appendDouble(s, "m.actualDataStartMs", m.actualDataStartMs);
    appendDouble(s, "m.actualDataEndMs", m.actualDataEndMs);
    appendDouble(s, "m.dataMinPrice", m.dataMinPrice); appendDouble(s, "m.dataMaxPrice", m.dataMaxPrice);
    appendDouble(s, "m.appendMs", m.appendMs); appendDouble(s, "m.tickSize", m.tickSize);
    appendInt(s, "m.gridWidth", m.gridWidth); appendInt(s, "m.gridHeight", m.gridHeight);
    appendInt(s, "m.filledColumns", m.filledColumns);
    appendRect(s, "m.drawRect", m.drawRect); appendRect(s, "m.srcRect", m.srcRect);
    appendDouble(s, "m.cellW", m.cellW); appendDouble(s, "m.cellH", m.cellH);
    appendDouble(s, "m.dpr", m.dpr);
    appendInt(s, "m.rowPxDev", m.rowPxDev); appendInt(s, "m.colPxDev", m.colPxDev);
    appendDouble(s, "m.timeOffset", double(m.timeOffset));
    appendInt(s, "m.valid", m.valid); appendInt(s, "m.viewportColumns", m.viewportColumns);
    const auto& c = f.raster;
    appendInt(s, "r.valid", c.valid); appendInt(s, "r.rowPx", c.rowPx); appendInt(s, "r.colPx", c.colPx);
    appendDouble(s, "r.dpr", c.dpr); appendInt(s, "r.widthDev", c.widthDev); appendInt(s, "r.heightDev", c.heightDev);
    appendInt(s, "r.topRowIndex", c.topRowIndex); appendInt(s, "r.leftColIndex", c.leftColIndex);
    appendDouble(s, "r.tfMs", c.tfMs); appendDouble(s, "r.tick", c.tick);
    appendDouble(s, "r.drawnStartMs", c.drawnStartMs); appendDouble(s, "r.drawnEndMs", c.drawnEndMs);
    appendDouble(s, "r.drawnMinPrice", c.drawnMinPrice); appendDouble(s, "r.drawnMaxPrice", c.drawnMaxPrice);
    appendInt(s, "r.free", c.free);
    appendDouble(s, "r.rowPxF", c.rowPxF); appendDouble(s, "r.colPxF", c.colPxF);
    appendDouble(s, "r.topF", c.topF); appendDouble(s, "r.leftF", c.leftF);
    return s;
}

QByteArray pixels(const QImage& image) {
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray bytes;
    bytes.reserve(rgba.width() * rgba.height() * 4);
    for (int y = 0; y < rgba.height(); ++y)
        bytes.append(reinterpret_cast<const char*>(rgba.constScanLine(y)), qsizetype(rgba.width()) * 4);
    return bytes;
}

QString hashOf(const QImage& image, const std::string& mapping) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(pixels(image));
    h.addData(QByteArrayView(mapping.data(), qsizetype(mapping.size())));
    return QString::fromLatin1(h.result().toHex());
}

void configureFixture(QTemporaryDir& dir) {
    writeRecording(dir, 4 * 60);
    lab::LabData::setLocalSymbolsForTest({"BTC-USD"});
    lab::LabData::configure(dir.path().toStdString(), epoch + 4 * kHourMs);
}

// One chart on one fresh scene. All time is manual: the zoom clock is the test's.
struct Harness {
    lab::OffscreenQuick scene;
    UnifiedGridRenderer* ugr = nullptr;
    CandlestickOverlayItem* candles = nullptr;
    TradeBubbleOverlayItem* bubbles = nullptr;
    CandleSeriesBuffer buffer;
    QString error;
    QImage image;
    qint64 clock = 1000;

    Harness() {
        if (!scene.create(QSize(640, 320), &error)) throw Failure("scene: " + error.toStdString());
        scene.window()->setColor(Qt::black);
        ugr = new UnifiedGridRenderer(scene.window()->contentItem());
        ugr->setSize(QSizeF(640, 320));
        ugr->setHeatmapService(&lab::LabData::instance().service());
        ugr->setActiveSymbol("BTC-USD");
        ugr->setTimeframe(int(minute));
        setSettings(false, false);
        ugr->setZoomClockForTest([this] { return clock; });
        std::vector<CandleSeriesBuffer::CandleBar> bars;
        for (int i = -10; i < 70; ++i) { // a deterministic walk, 1m bars around the 40 min view
            const double open = 100150 + 45.0 * std::sin(i * 0.31) + 6.0 * (i % 5);
            const double close = open + ((i % 3 == 0) ? -22.0 - i % 7 : 18.0 + i % 11);
            const double high = std::max(open, close) + 9 + i % 13, low = std::min(open, close) - 8 - i % 9;
            const qint64 t = viewLo + i * minute;
            bars.push_back({t, t + minute, open, high, low, close, 1, true, 0, false});
        }
        buffer.applyHistory("BTC-USD", 60, bars);
        candles = new CandlestickOverlayItem(scene.window()->contentItem());
        candles->setSize(ugr->size());
        candles->setMappingProvider(ugr);
        candles->setCandleBuffer(&buffer);
        candles->setSymbol("BTC-USD");
        candles->setTimeframeSec(60);
        candles->setZ(2);
    }
    void addBubbles() {
        bubbles = new TradeBubbleOverlayItem; // parented after construction, as QML does: the item connects on windowChanged
        bubbles->setParentItem(scene.window()->contentItem());
        bubbles->setSize(ugr->size());
        bubbles->setRenderer(ugr);
        bubbles->setZ(2.5);
        // QQuickRenderControl::sync() does not emit afterSynchronizing (the window render loops do), so the
        // bubble node would never take the frame's data: deliver it between sync and render, as a loop would.
        QObject::connect(scene.window(), &QQuickWindow::beforeRendering, scene.window(), [this] {
            QMetaObject::invokeMethod(scene.window(), "afterSynchronizing", Qt::DirectConnection);
        }, Qt::DirectConnection);
    }
    ~Harness() {
        candles->setCandleBuffer(nullptr);
        candles->setMappingProvider(nullptr);
        if (bubbles) bubbles->setRenderer(nullptr);
        delete ugr; // the overlays are children of the window's content item: the scene frees them
        QString ignored;
        scene.renderFrame(&ignored);
    }
    void setSettings(bool labels, bool trades) {
        heatmap::HeatmapChartSettings s;
        s.palettePreset = "Fire";
        s.sensitivityMin = 0.001;
        s.sensitivityMax = 1;
        s.showLabels = labels;
        s.showTrades = trades;
        ugr->setHeatmapChartSettings(s);
    }
    heatmap::gpu::HeatmapGpuLayer& layer() const { return *ugr->gpuHeatmapLayer(); }
    void frame() {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        ugr->update();
        image = scene.renderFrame(&error);
        if (image.isNull()) throw Failure("render failed: " + error.toStdString());
    }
    // Settled: the tile node is not crossfading or holding (steady_clock fades make an
    // unsettled frame non-deterministic), and the picture is identical for 4 frames.
    // `tile` is false for scenes whose heatmap layer is off. `glide` keeps a held glide.
    void settle(const char* what, bool tile = true, bool glide = false, bool labels = false) {
        QElapsedTimer timer;
        timer.start();
        QByteArray previous;
        int streak = 0;
        while (timer.elapsed() < 30000) {
            frame();
            bool ok = true;
            if (tile) {
                const auto& st = layer().tileStats();
                ok = layer().settled() && !st.crossfading.load() && !st.holding.load();
            }
            if (labels && (ugr->gpuLabelGlyphs().empty() || ugr->gpuLabelLayout().stats().labels == 0)) ok = false;
            if (!glide && ugr->zoomGliding()) { clock += 20; ok = false; }
            const QByteArray now = pixels(image);
            streak = ok && now == previous ? streak + 1 : 0;
            previous = now;
            if (streak >= 4) return;
        }
        throw Failure(std::string("did not settle: ") + what);
    }
    void view(int64_t lo, int64_t hi, double pmin, double pmax) { ugr->setViewport(lo, hi, pmin, pmax); }
};

struct Scene {
    const char* name;
    std::function<void(Harness&)> run;
};

TradeOverlayGrid gridOf(int64_t start, int64_t end, double maxPrice, double tick) {
    TradeOverlayGrid g;
    g.startMs = start; g.endMs = end; g.maxPrice = maxPrice; g.tick = tick; g.generation = 1;
    return g;
}

void rest(Harness& h) {
    h.view(viewLo, viewHi, 99'900, 100'300);
    h.ugr->setAutoPriceScale(true);
    h.settle("rest");
}
void useViewport(Harness& h) { h.view(viewLo, viewHi, 99'900, 100'300); h.settle("viewport"); }

std::vector<Scene> scenes() {
    std::vector<Scene> s;
    s.push_back({"01.rest-auto-candles", rest});
    s.push_back({"02.manual-tick", [](Harness& h) {
        h.view(viewLo, viewHi, 99'900, 100'300);
        auto settings = heatmap::HeatmapChartSettings();
        settings.palettePreset = "Fire"; settings.sensitivityMin = 0.001; settings.sensitivityMax = 1;
        settings.showLabels = false;
        settings.tickMode = heatmap::TickMode::Manual;
        settings.manualTick = 500;
        h.ugr->setHeatmapChartSettings(settings, true);
        h.settle("manual tick");
    }});
    s.push_back({"03.mid-glide-65ms", [](Harness& h) {
        useViewport(h);
        h.ugr->zoomClicks(1, 320, 160);
        h.clock += 65;
        h.settle("mid-glide", true, true);
        if (!h.ugr->zoomGliding()) throw Failure("scene 3 is not mid-glide at 65 ms");
    }});
    s.push_back({"04.drag-0.3px", [](Harness& h) {
        useViewport(h);
        h.ugr->beginPanAt(320, 160);
        h.ugr->updatePanAt(320.3, 160);
        h.settle("drag");
    }});
    s.push_back({"05.follow-live-step", [](Harness& h) {
        useViewport(h);
        h.ugr->enableAutoScroll(true);
        h.ugr->followLiveForTest(epoch + 2 * kHourMs + 45 * minute);
        h.settle("follow live");
    }});
    for (const char* layout : {"split", "collapsed"}) {
        s.push_back({layout[0] == 's' ? "06a.tpo-split" : "06b.tpo-collapsed", [layout](Harness& h) {
            useViewport(h);
            h.ugr->setTpoLayout(layout);
            h.ugr->setTpoLayerEnabled(true);
            h.settle("tpo plain", false);
            const QByteArray plain = pixels(h.image);
            const auto grid = gridOf(viewLo, viewLo + 24 * kHourMs, 100'300, 10);
            for (int x = 0; x < 12; ++x) {
                QByteArray letters(40, '\0');
                for (int r = 4 + x; r < 30 - x / 2; ++r) letters[r] = char('A' + (r + x) % 6);
                emit h.ugr->getDataProcessor()->tpoColumnReady(x, 288, 40, letters, grid.startMs, grid.endMs, 5 * minute, grid);
            }
            h.settle("tpo", false);
            if (pixels(h.image) == plain) throw Failure("the TPO columns drew nothing");
        }});
    }
    s.push_back({"07.volume-profile", [](Harness& h) {
        useViewport(h);
        h.ugr->setVolumeProfileLayerEnabled(true);
        h.settle("vp plain", false);
        const QByteArray plain = pixels(h.image);
        std::vector<float> bins(40);
        for (int i = 0; i < 40; ++i) bins[size_t(i)] = float(20 + 180 * std::exp(-0.5 * std::pow((i - 18) / 6.0, 2)));
        VolumeProfileState::Snapshot snap;
        snap.sessionStartMs = viewLo; snap.sessionEndMs = viewLo + 24 * kHourMs;
        snap.minPrice = 99'900; snap.maxPrice = 100'300; snap.tickSize = 10; snap.gridHeight = 40;
        snap.va.valid = true; snap.va.pocRow = 18; snap.va.vahRow = 12; snap.va.valRow = 25;
        snap.va.pocPrice = 100'300 - 18.5 * 10; snap.va.vahPrice = 100'300 - 12 * 10; snap.va.valPrice = 100'300 - 26 * 10;
        snap.va.totalVolume = 4000; snap.generation = 1;
        emit h.ugr->getDataProcessor()->volumeProfileReady(bins, snap);
        h.settle("vp", false);
        if (pixels(h.image) == plain) throw Failure("the volume profile drew nothing");
    }});
    s.push_back({"08.footprint", [](Harness& h) {
        useViewport(h);
        h.ugr->setFootprintLayerEnabled(true);
        h.settle("footprint plain", false);
        const QByteArray plain = pixels(h.image);
        const auto grid = gridOf(viewLo, viewLo + 40 * minute, 100'300, 4);
        for (int x = 0; x < 40; ++x) {
            QByteArray column(100 * 2, '\0');
            for (int r = 0; r < 100; ++r) {
                const int v = int(16000.0 * std::sin((r + 3 * x) * 0.21) * std::cos(x * 0.17));
                const uint16_t u = uint16_t(int16_t(v));
                column[2 * r] = char(u & 0xff); column[2 * r + 1] = char(u >> 8);
            }
            emit h.ugr->getDataProcessor()->footprintColumnReady(x, 40, 100, column, grid);
        }
        h.settle("footprint", false);
        if (pixels(h.image) == plain) throw Failure("the footprint columns drew nothing");
    }});
    s.push_back({"09.labels", [](Harness& h) {
        h.view(viewLo, viewLo + 10 * minute, 100'000, 100'016); // $1 cells of 64 x 20 px: room for labels
        h.setSettings(true, false);
        h.settle("labels", true, false, true);
    }});
    s.push_back({"10.trade-bubbles", [](Harness& h) {
        useViewport(h);
        h.setSettings(false, true);
        h.settle("bubbles plain");
        const QByteArray plain = pixels(h.image);
        for (int i = 0; i < 24; ++i) {
            Trade t{};
            t.product_id = "BTC-USD";
            t.price = 99'950 + 12.0 * ((i * 7) % 28);
            t.size = 0.2 + 0.35 * (i % 6);
            t.side = (i % 3) ? AggressorSide::Buy : AggressorSide::Sell;
            t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(viewLo + 90'000 * i + 20'000));
            t.trade_id = std::to_string(1000 + i);
            h.ugr->addTrade(t);
        }
        // Items are added after the data: a bubble node that starts empty and is filled later is not
        // drawn on Qt 6.11.2 / Metal here (zero-vertex node, see the S0b hand-off).
        h.addBubbles();
        h.settle("bubbles");
        if (pixels(h.image) == plain) throw Failure("the trade bubbles drew nothing");
    }});
    s.push_back({"11.after-1m-to-15m", [](Harness& h) {
        useViewport(h);
        h.ugr->setTimeframe(int(15 * minute));
        h.settle("15m");
    }});
    s.push_back({"12.rest-dpr2", [](Harness& h) {
        useViewport(h);
        if (!h.scene.setDevicePixelRatio(2.0, &h.error)) throw Failure("dpr 2: " + h.error.toStdString());
        emit h.scene.window()->screenChanged(h.scene.window()->screen());
        h.settle("dpr 2");
    }});
    return s;
}

QString outDir() {
#ifdef SENTINEL_GOLDEN_OUT_DIR
    return QStringLiteral(SENTINEL_GOLDEN_OUT_DIR);
#else
    return QDir::tempPath() + "/golden-out";
#endif
}
} // namespace

int main(int argc, char** argv) {
    bool bless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bless") bless = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QString error;
    lab::selectQuickSceneGraph(&error);
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    Q_INIT_RESOURCE(heatmap_shaders); // trade_bubble / msdf / footprint qsb: static lib resource
    try {
        std::printf("[sentinel] %s\n", lab::describeRhi().toStdString().c_str());
        const bool owner =
#ifdef Q_OS_MACOS
            true;
#else
            false;
#endif
        if (const QString why = lab::gpuUnavailableReason(); !why.isEmpty()) {
            if (owner) throw Failure("golden: no GPU on the Mac: " + why.toStdString());
            std::fprintf(stderr, "ChartGolden: SKIPPED (GPU case skipped: %s)\n", why.toStdString().c_str());
            return 77;
        }
        QTemporaryDir dir;
        if (!dir.isValid()) throw Failure("temporary fixture directory failed");
        configureFixture(dir);

        QString key;
        std::vector<std::pair<QString, QImage>> images;
        QJsonObject mine;
        {
            const auto list = scenes();
            for (const auto& sc : list) {
                Harness h;
                if (key.isEmpty()) {
                    const auto info = h.scene.rhi()->driverInfo();
                    key = QString::fromLatin1(h.scene.rhi()->backendName()) + ":" + QString::fromUtf8(info.deviceName);
                    std::printf("ChartGolden: device key '%s' (%s)\n", key.toStdString().c_str(),
                                lab::describeRhi().toStdString().c_str());
                }
                sc.run(h);
                // The picture and the mapping are read from the last settled frame.
                const auto hash = hashOf(h.image, serializeFrame(h.ugr->currentFrameContext()));
                mine[sc.name] = hash;
                images.emplace_back(QString::fromLatin1(sc.name), h.image);
                if (qEnvironmentVariableIsSet("SENTINEL_GOLDEN_DUMP_ALL")) { // look at every scene
                    QDir().mkpath(outDir());
                    h.image.save(outDir() + "/" + sc.name + ".png");
                }
                std::printf("ChartGolden: %s %s\n", sc.name, hash.toStdString().c_str());
            }
        }
        const bool required = owner && key == kRequiredKey;
        if (owner && !required)
            std::fprintf(stderr, "ChartGolden: WARNING this Mac is '%s', not '%s': baselines are checked when present\n",
                         key.toStdString().c_str(), kRequiredKey);

        QFile file(QStringLiteral(SENTINEL_SOURCE_DIR "/tests/render/golden/frames.json"));
        QJsonObject keys;
        if (file.open(QIODevice::ReadOnly)) {
            keys = QJsonDocument::fromJson(file.readAll()).object().value("keys").toObject();
            file.close();
        } else if (!bless && required) {
            throw Failure("golden: required baseline missing for " + key.toStdString() + "/(manifest frames.json)");
        }
        if (bless) {
            keys[key] = mine;
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw Failure("cannot write frames.json");
            file.write(QJsonDocument(QJsonObject{{"version", 1}, {"keys", keys}}).toJson(QJsonDocument::Indented));
            std::puts("ChartGolden: blessed");
            return 0;
        }
        const QJsonObject expected = keys.value(key).toObject();
        int failures = 0;
        for (const auto& [name, image] : images) {
            const QString want = expected.value(name).toString();
            if (want.isEmpty()) {
                if (required || !keys.value(key).isUndefined()) {
                    std::fprintf(stderr, "golden: required baseline missing for %s/%s\n", key.toStdString().c_str(),
                                 name.toStdString().c_str());
                    ++failures;
                }
                continue;
            }
            if (want != mine.value(name).toString()) {
                QDir().mkpath(outDir());
                image.save(outDir() + "/" + name + ".png");
                std::fprintf(stderr, "golden: MISMATCH %s/%s expected %s actual %s (png in %s)\n", key.toStdString().c_str(),
                             name.toStdString().c_str(), want.toStdString().c_str(),
                             mine.value(name).toString().toStdString().c_str(), outDir().toStdString().c_str());
                ++failures;
            }
        }
        if (required || !expected.isEmpty())
            for (const QString& name : expected.keys())
                if (!mine.contains(name)) {
                    std::fprintf(stderr, "golden: scene in manifest not produced: %s\n", name.toStdString().c_str());
                    ++failures;
                }
        if (!owner && expected.isEmpty()) {
            std::fprintf(stderr, "ChartGolden: SKIPPED (no baseline for '%s' on this machine)\n", key.toStdString().c_str());
            return 77;
        }
        if (failures) return 1;
        std::puts("ChartGolden: PASS");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ChartGolden: %s\n", e.what());
        return 1;
    }
}
