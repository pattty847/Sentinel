#include "lab/B1Bench.hpp"
#include "lab/Bench.hpp"
#include "lab/LabChunks.hpp"
#include "lab/LabItem.hpp"
#include "lab/RhiBackend.hpp"
#include <QCommandLineParser>
#include <QDateTime>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
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
                    QByteArray(argv[i]) == "--b1-bench" ||
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
    parser.addOption({"layer", "Recording layer: near or deep", "layer", "near"});
    parser.addOption({"synthetic", "Generate this many synthetic entries", "entries"});
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
    parser.addOption({"prep", "B1 preparation mode: full (S4/T lab), viewport (V), whole-chunk (W, GPU-binned tiles), "
                              "whole-chunk-cpu (W, CPU-binned tiles), hybrid (resident tile sources, rows around the view)", "mode", "full"});
    parser.addOption({"charts", "Lab items side by side (1-4); extra items show 5m, 1h, 15m of the same symbol", "n", "1"});
    parser.addOption({"b1-bench", "Headless B1: scripted pan/zoom sessions in V and W modes; prints a table, writes JSON", "json"});
    parser.addOption({"b1-quick", "With --b1-bench: a reduced matrix (1x only, 1m and 1h)"});
    parser.addOption({"end-utc", "Pin the recording's end (exclusive), e.g. 2026-09-30T00:00:00Z: viewport/whole-chunk/hybrid "
                                 "modes and --b1-bench see only this closed range (default: live)", "time"});
    parser.addOption({"b1-modes", "With --b1-bench: comma-separated prep modes to run (default: all four)", "modes"});
    parser.addOption({"window-screenshot", "Interactive window: once every chart has settled, grab the whole window "
                                           "(controls and debug panel included) to this PNG and exit", "path"});
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
    options.prep = parser.value("prep");
    if (!lab::parsePrepMode(options.prep)) return 2;
    options.charts = parser.value("charts").toInt(&ok);
    if (!ok || options.charts < 1 || options.charts > 4) return 2;
    if (parser.isSet("end-utc")) {
        const auto end = QDateTime::fromString(parser.value("end-utc"), Qt::ISODate);
        if (!end.isValid()) return 2;
        lab::setPinnedEndMs(end.toMSecsSinceEpoch());
    }
    if (parser.isSet("b1-bench")) return lab::runB1Bench(parser.value("b1-bench"), parser.isSet("b1-quick"),
                                                             parser.isSet("b1-modes") ? parser.value("b1-modes").split(',') : QStringList{});
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
    engine.rootContext()->setContextProperty("initialLayer", layer);
    engine.rootContext()->setContextProperty("initialSynthetic", synthetic);
    engine.rootContext()->setContextProperty("initialTf", tf);
    engine.rootContext()->setContextProperty("initialPrep", options.prep);
    engine.rootContext()->setContextProperty("chartCount", options.charts);
    engine.load(QUrl(QStringLiteral("qrc:/lab/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 2;
    if (auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab"))) {
        item->setPersistTickMemory(true); // Manual ticks per (symbol, timeframe) survive restarts
        lab::applyTickOptions(*item, options);
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
                          << " prep=" << m.value("prep").toString().toStdString() << " tick=" << m.value("tick").toDouble()
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
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [item, &app, hours, layer, tf, launched] {
            const auto metrics = item->metrics();
            if (metrics.value("firstFrameMs").toDouble() <= 0) return;
            const auto presentedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - launched).count();
            const QJsonObject result{{"hours", hours}, {"layer", layer},
                                     {"entries", metrics.value("entries").toDouble()},
                                     {"load_ms", metrics.value("loadMs").toDouble()},
                                     {"compose_ms", metrics.value("composeMs").toDouble()},
                                     {"build_ms", metrics.value("buildMs").toDouble()},
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
