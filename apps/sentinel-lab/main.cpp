#include "lab/Bench.hpp"
#include "lab/LabData.hpp"
#include "lab/LabItem.hpp"
#include "lab/LabSources.hpp"
#include "lab/RhiBackend.hpp"
#include "lab/S5Bench.hpp"
#include "ConfigLoader.hpp"
#include "render/heatmap/HeatmapSettingsStore.hpp"
#include "config/ConfigTypes.hpp"
#include <QCommandLineParser>
#include <QDateTime>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QTimeZone>
#include <QDir>
#include <QFileInfo>
#include <QtQml/qqml.h>
#include <algorithm>
#include <functional>
#include <chrono>
#include <iostream>

int main(int argc, char **argv) {
    const auto launched = std::chrono::steady_clock::now();
    bool headless = false;
    for (int i = 1; i < argc; ++i)
        headless |= QByteArray(argv[i]) == "--bench" || QByteArray(argv[i]) == "--screenshot" ||
                    QByteArray(argv[i]) == "--s5-bench" ||
                    QByteArray(argv[i]) == "--tick-sweep" || QByteArray(argv[i]) == "--tick-change-frames";
    // Offscreen unless set: Vulkan needs a real platform plugin (QT_QPA_PLATFORM=windows|xcb).
    if (headless && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // The offscreen QPA reads fonts from Qt's lib/fonts, which Qt no longer ships:
    // without this the stamped debug text renders as empty boxes.
    if (headless && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR").isEmpty() ? QByteArray("C:/Windows/Fonts") : qgetenv("WINDIR") + "/Fonts");
#endif
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sentinel-lab"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"bench", "Run 200 headless bin passes per grid (1x, 2x) on SENTINEL_RHI_BACKEND (or the platform default) and emit JSON"});
    parser.addOption({"first-paint", "Exit after the first lab frame and emit launch timing JSON"});
    parser.addOption({"hours", "Hours back from now", "hours", "24"});
    parser.addOption({"layer", "--bench only: recording layer near or deep", "layer", "near"});
    parser.addOption({"synthetic", "--bench only: generate this many synthetic entries", "entries"});
    parser.addOption({"tf", "Exact timeframe in minutes (1, 5, 15, 60, 240, 1440, or custom)", "minutes", "1"});
    parser.addOption({"screenshot", "Render the lab item offscreen (real scene graph) once settled, save PNG", "path"});
    parser.addOption({"pan-columns", "With --screenshot: pan by this many (fractional) columns first", "columns", "0"});
    parser.addOption({"tick-mode", "Price tick mode: auto (default) or manual", "mode", "auto"});
    parser.addOption({"tick", "Manual tick in price units, a {1, 2, 2.5, 5} x 10^k preset (implies manual)", "price"});
    parser.addOption({"hysteresis", "Auto tick hysteresis h (0..0.9; lab presets 0, 0.15, 0.25, 0.4)", "h", "0.25"});
    parser.addOption({"min-row-px", "Auto: smallest row height in physical pixels", "px", "2"});
    parser.addOption({"zoom-rows-px", "Once loaded: zoom price so one commonTick() row is this many physical px", "px"});
    parser.addOption({"no-crossfade", "Hard switch at a tick change (default: 150 ms crossfade, spec rule 8)"});
    parser.addOption({"tick-sweep", "Headless E1: zoom through row heights for each h, log every tick change (JSON)"});
    parser.addOption({"tick-change-frames", "Headless E2: force an Auto tick change, save ~25 ms frames for 300 ms", "dir"});
    parser.addOption({"charts", "Lab charts side by side (1-4); extra charts show 5m, 1h, 15m of the same symbol", "n", "1"});
    parser.addOption({"s5-bench", "Headless S5c bench of the production path vs the B1 hybrid numbers; prints a table, writes JSON", "json"});
    parser.addOption({"s5-quick", "With --s5-bench: a reduced matrix (1x only, 1m and 1h)"});
    parser.addOption({"end-utc", "Pin the recording's end (exclusive), e.g. 2026-09-30T00:00:00Z: the lab and --s5-bench "
                                 "see only this closed range (default: live)", "time"});
    parser.addOption({"band-edges", "E4: draw the edges of the finest source's coverage band (the near band)"});
    parser.addOption({"center-price", "With --screenshot: centre the view on this price once settled", "price"});
    parser.addOption({"price-span", "View height in price units: with --screenshot and --center-price, or (interactive) "
                                    "around the recent mid", "price"});
    parser.addOption({"gpu-cap-mb", "Per-chart GPU cap in MiB (default 320)", "mb"});
    parser.addOption({"window-screenshot", "Interactive window: once every chart has settled, grab the whole window "
                                           "(controls and debug panel included) to this PNG and exit", "path"});
    parser.addOption({"server", "Chunks and the live edge from a running sentinel-server (host, port and CA from "
                                "config/client_config.yaml) instead of the local recording (S5L-c)"});
    parser.addOption({"host", "With --server: host (default: config server.host)", "host"});
    parser.addOption({"port", "With --server: port (default: config server.port)", "port"});
    parser.addOption({"ca", "With --server: CA file verifying the server (default: config server.ca_file)", "path"});
    parser.addOption({"window-minutes", "Interactive: the initial time span in minutes instead of --hours", "minutes"});
    parser.addOption({"live-run", "Interactive window: exit after this many seconds and print the live-edge "
                                  "telemetry (latency p50/p95, versions, uploads) as JSON", "seconds"});
    parser.addOption({"live-shots", "With --live-run: grab the window at +10 s and +40 s into every minute and "
                                    "2 s after each minute rollover into this directory", "dir"});
    parser.process(app);
    bool ok = false;
    const int hours = parser.value("hours").toInt(&ok);
    if (!ok || hours < 1 || hours > 24 * 30) return 2;
    const QString layer = parser.value("layer");
    if (layer != "near" && layer != "deep") return 2;
    uint32_t synthetic = 0;
    if (parser.isSet("synthetic")) {
        const auto value = parser.value("synthetic").toUInt(&ok);
        if (!ok || value < 1 || value > 30'000'000) return 2;
        synthetic = value;
    }
    const int tf = parser.value("tf").toInt(&ok);
    if (!ok || tf < 1 || tf > 1440) return 2;
    lab::LabRunOptions options;
    options.hours = hours;
    options.layer = layer;
    options.synthetic = synthetic;
    options.tfMinutes = tf;
    const QString mode = parser.value("tick-mode");
    if (mode != "auto" && mode != "manual") return 2;
    options.manual = mode == "manual";
    if (parser.isSet("tick")) {
        options.tick = parser.value("tick").toDouble(&ok);
        if (!ok || !(options.tick > 0)) return 2;
    }
    options.hysteresis = parser.value("hysteresis").toDouble(&ok);
    if (!ok || options.hysteresis < 0 || options.hysteresis > 0.9) return 2;
    options.hysteresisSet = parser.isSet("hysteresis");
    options.minRowPx = parser.value("min-row-px").toDouble(&ok);
    if (!ok || options.minRowPx < 0.5 || options.minRowPx > 32) return 2;
    if (parser.isSet("zoom-rows-px")) {
        options.zoomRowsPx = parser.value("zoom-rows-px").toDouble(&ok);
        if (!ok || !(options.zoomRowsPx > 0)) return 2;
    }
    options.crossfade = !parser.isSet("no-crossfade");
    options.panColumns = parser.value("pan-columns").toDouble();
    options.charts = parser.value("charts").toInt(&ok);
    if (!ok || options.charts < 1 || options.charts > 4) return 2;
    options.bandEdges = parser.isSet("band-edges");
    if (parser.isSet("center-price")) {
        options.centerPrice = parser.value("center-price").toDouble(&ok);
        if (!ok || !(options.centerPrice > 0)) return 2;
    }
    if (parser.isSet("price-span")) {
        options.priceSpan = parser.value("price-span").toDouble(&ok);
        if (!ok || !(options.priceSpan > 0)) return 2;
    }
    if (parser.isSet("gpu-cap-mb")) {
        const double mb = parser.value("gpu-cap-mb").toDouble(&ok);
        if (!ok || !(mb > 0)) return 2;
        options.gpuCapBytes = uint64_t(mb * 1048576.0);
    }
    ClientConfig config;
    ConfigLoader::loadClientConfig("config/client_config.yaml", &config);
    ConfigLoader::loadClientConfig("config/.client_config.yaml", &config);
    const auto budgets = heatmap::HeatmapSettingsStore{}.loadBudgets(config.heatmap);
    int64_t pinnedEndMs = 0;
    if (parser.isSet("end-utc")) {
        const auto end = QDateTime::fromString(parser.value("end-utc"), Qt::ISODate);
        if (!end.isValid()) return 2;
        pinnedEndMs = end.toMSecsSinceEpoch();
    }
    lab::LabData::configure({}, pinnedEndMs, budgets);
    const bool server = parser.isSet("server");
    if (server) {
        if (parser.isSet("end-utc")) return 2; // a pinned end is the local recording's
        lab::LabData::Server endpoint{config.server.host, config.server.port, config.server.caFile};
        if (parser.isSet("host")) endpoint.host = parser.value("host").toStdString();
        if (parser.isSet("port")) endpoint.port = parser.value("port").toStdString();
        if (parser.isSet("ca")) endpoint.caFile = parser.value("ca").toStdString();
        if (!endpoint.caFile.empty() && !QFileInfo::exists(QString::fromStdString(endpoint.caFile))) {
            std::cerr << "CA file not found: " << endpoint.caFile << " (pass --ca)" << std::endl;
            return 2;
        }
        lab::LabData::configureServer(endpoint);
    }
    if (parser.isSet("s5-bench")) return lab::runS5Bench(parser.value("s5-bench"), parser.isSet("s5-quick"));
    if (parser.isSet("bench")) return lab::runBench(hours, layer, synthetic, tf);
    if (parser.isSet("tick-sweep")) return lab::runTickSweep(options);
    if (parser.isSet("tick-change-frames")) return lab::runTickChangeSequence(options, parser.value("tick-change-frames"));
    if (parser.isSet("screenshot")) return lab::runScreenshot(options, parser.value("screenshot"));
    const auto rhi = lab::selectedRhiBackend();
    if (!rhi.valid) {
        std::cerr << rhi.error.toStdString() << std::endl;
        return 2;
    }
    QQuickWindow::setGraphicsApi(rhi.backend.graphicsApi);
    qmlRegisterType<lab::LabItem>("Sentinel.Lab", 1, 0, "BinLab");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("initialHours", hours);
    engine.rootContext()->setContextProperty("initialTf", tf);
    engine.rootContext()->setContextProperty("chartCount", options.charts);
    engine.rootContext()->setContextProperty("initialBandEdges", options.bandEdges);
    engine.load(QUrl(QStringLiteral("qrc:/lab/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 2;
    if (auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab"))) {
        item->setPersistTickMemory(true); // Manual ticks per (symbol, timeframe) survive restarts
        lab::applyTickOptions(*item, options);
    }
    if (parser.isSet("window-minutes"))
        if (auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab")))
            item->setInitialTimeSpanMs(parser.value("window-minutes").toDouble() * 60'000.0);
    if (parser.isSet("price-span") && !parser.isSet("center-price"))
        if (auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab")))
            item->setInitialPriceSpan(options.priceSpan); // interactive: around the recent mid
    if (server) { // every chart follows the live edge until a pan
        std::function<void(QQuickItem *)> follow = [&](QQuickItem *item) {
            if (auto *labItem = qobject_cast<lab::LabItem *>(item)) labItem->setFollowLive(true);
            for (auto *child : item->childItems()) follow(child);
        };
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) follow(window->contentItem());
    }
    if (parser.isSet("live-run")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab"));
        const int seconds = parser.value("live-run").toInt(&ok);
        if (!window || !item || !ok || seconds < 5) return 2;
        const QString dir = parser.value("live-shots");
        if (!dir.isEmpty()) {
            if (QString why; !lab::labOutputAllowed(dir + "/x.png", &why)) {
                std::cerr << why.toStdString() << std::endl;
                return 2;
            }
            QDir().mkpath(dir);
        }
        // Shots at +10 s and +40 s into each minute and 2 s after each rollover
        // (UTC), named by tf and time; the frame count shows the forming column
        // updated between them.
        auto *tick = new QTimer(&app);
        auto lastSecond = std::make_shared<int64_t>(-1);
        QObject::connect(tick, &QTimer::timeout, &app, [window, item, dir, lastSecond, tf] {
            const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
            const int64_t second = nowMs / 1000;
            if (second == *lastSecond) return;
            *lastSecond = second;
            const int into = int(second % 60);
            if (dir.isEmpty() || (into != 2 && into != 10 && into != 40)) return;
            const auto m = item->metrics();
            const QString name = QStringLiteral("%1/s5l-live-%2m-%3-%4.png")
                .arg(dir).arg(tf)
                .arg(QDateTime::fromMSecsSinceEpoch(nowMs, QTimeZone::UTC).toString(QStringLiteral("HHmmss")))
                .arg(into == 2 ? QStringLiteral("rollover") : QStringLiteral("t%1").arg(into));
            QString why;
            if (!lab::labOutputAllowed(name, &why)) return;
            const bool saved = window->grabWindow().save(name, "PNG");
            std::cerr << "live shot " << name.toStdString() << (saved ? "" : " FAILED")
                      << " version=" << m.value("liveVersion").toULongLong()
                      << " L=" << m.value("liveL").toString().toStdString()
                      << " end=" << m.value("liveEnd").toString().toStdString()
                      << " from=" << m.value("liveFrom").toString().toStdString()
                      << " ageMs=" << m.value("liveDataAgeMs").toDouble()
                      << " publishToDrawMs=" << m.value("livePublishToDrawMs").toDouble()
                      << " liveBuffersCreated=" << m.value("liveBufferCreations").toULongLong()
                      << " uploads=" << m.value("liveUploads").toULongLong()
                      << " connection=" << m.value("connection").toString().toStdString() << std::endl;
        });
        tick->start(100);
        QTimer::singleShot(seconds * 1000, &app, [item, &app, seconds, tf] {
            const auto m = item->metrics();
            QJsonObject out;
            for (const char *key : {"connection", "liveVersion", "livePublished", "liveSamples", "livePublishP50",
                                    "livePublishP95", "liveAgeP50", "liveAgeP95", "liveUploads", "liveBinPasses",
                                    "liveBufferCreations", "liveSets", "liveL", "liveEnd", "liveFrom", "liveE",
                                    "liveSpanMin", "livePublications", "liveComposeMs", "liveIntervalMs", "fps",
                                    "frameMs", "loadingSlots", "errors", "missingDraws", "tick"})
                out.insert(QString::fromLatin1(key), QJsonValue::fromVariant(m.value(QString::fromLatin1(key))));
            out.insert("seconds", seconds);
            out.insert("timeframe_minutes", tf);
            std::cout << QJsonDocument(out).toJson(QJsonDocument::Compact).constData() << std::endl;
            app.exit(0);
        });
    }
    if (options.gpuCapBytes) { // every chart
        std::function<void(QQuickItem *)> apply = [&](QQuickItem *item) {
            if (auto *labItem = qobject_cast<lab::LabItem *>(item)) labItem->setGpuCapBytes(options.gpuCapBytes);
            for (auto *child : item->childItems()) apply(child);
        };
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) apply(window->contentItem());
    }
    if (parser.isSet("window-screenshot")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window) return 2;
        const QString path = parser.value("window-screenshot");
        if (QString why; !lab::labOutputAllowed(path, &why)) { // before the run; again before the write
            std::cerr << why.toStdString() << std::endl;
            return 2;
        }
        auto *poll = new QTimer(&app);
        QObject::connect(poll, &QTimer::timeout, &app, [window, path, &app, poll] {
            QList<lab::LabItem *> items;
            std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
                if (auto *labItem = qobject_cast<lab::LabItem *>(item)) items.push_back(labItem);
                for (auto *child : item->childItems()) collect(child);
            };
            collect(window->contentItem());
            if (items.isEmpty() || !std::all_of(items.begin(), items.end(), [](lab::LabItem *i) { return i->settled(); }))
                return;
            poll->stop();
            for (auto *item : items) {
                const auto m = item->metrics();
                std::cerr << item->objectName().toStdString() << " tf=" << m.value("timeframeMinutes").toInt()
                          << " tick=" << m.value("tick").toDouble()
                          << " gpuMB=" << m.value("gpuBytes").toDouble() / 1048576 << " span_min=" << m.value("timeSpanMin").toDouble()
                          << " status=" << item->status().toStdString() << "\n";
            }
            QTimer::singleShot(700, &app, [window, path, &app] { // let the panel refresh its metrics
                QString why;
                if (!lab::labOutputAllowed(path, &why)) {
                    std::cerr << why.toStdString() << std::endl;
                    app.exit(2);
                    return;
                }
                app.exit(window->grabWindow().save(path, "PNG") ? 0 : 2);
            });
        });
        poll->start(100);
        QTimer::singleShot(120'000, &app, [&app] { app.exit(2); });
    }
    if (parser.isSet("first-paint")) {
        auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!item || !window) return 2;
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [item, &app, hours, tf, launched] {
            const auto metrics = item->metrics();
            if (metrics.value("firstFrameMs").toDouble() <= 0) return;
            const auto presentedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - launched).count();
            const QJsonObject result{{"hours", hours},
                                     {"first_painted_ms", metrics.value("firstFrameMs").toDouble()},
                                     {"launch_to_first_frame_ms", presentedMs},
                                     {"timeframe_minutes", tf}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
            app.exit(0);
        }, Qt::QueuedConnection);
        QTimer::singleShot(60'000, &app, [&app] { app.exit(2); });
    }
    return app.exec();
}
