#include "lab/Bench.hpp"
#include "lab/LabItem.hpp"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtQml/qqml.h>
#include <chrono>
#include <iostream>

int main(int argc, char **argv) {
    const auto launched = std::chrono::steady_clock::now();
    bool benchmark = false;
    for (int i = 1; i < argc; ++i) benchmark |= QByteArray(argv[i]) == "--bench";
    if (benchmark) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sentinel-lab"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"bench", "Run 200 headless Metal bin passes and emit JSON"});
    parser.addOption({"first-paint", "Exit after the first lab frame and emit launch timing JSON"});
    parser.addOption({"hours", "Hours back from now", "hours", "24"});
    parser.addOption({"layer", "Recording layer: near or deep", "layer", "near"});
    parser.addOption({"synthetic", "Generate this many synthetic entries", "entries"});
    parser.process(app);
    bool ok = false;
    const int hours = parser.value("hours").toInt(&ok);
    if (!ok || hours < 1 || hours > 24 * 30) return 2;
    const QString layer = parser.value("layer");
    if (layer != "near" && layer != "deep") return 2;
    uint32_t synthetic = 0;
    if (parser.isSet("synthetic")) {
        const auto value = parser.value("synthetic").toUInt(&ok);
        if (!ok || value < 1 || value > 100'000'000) return 2;
        synthetic = value;
    }
    if (parser.isSet("bench")) return lab::runBench(hours, layer, synthetic);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    qmlRegisterType<lab::LabItem>("Sentinel.Lab", 1, 0, "BinLab");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("initialHours", hours);
    engine.rootContext()->setContextProperty("initialLayer", layer);
    engine.rootContext()->setContextProperty("initialSynthetic", synthetic);
    engine.load(QUrl(QStringLiteral("qrc:/lab/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 2;
    if (parser.isSet("first-paint")) {
        auto *item = engine.rootObjects().first()->findChild<lab::LabItem *>(QStringLiteral("binLab"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!item || !window) return 2;
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [item, &app, hours, layer, launched] {
            const auto metrics = item->metrics();
            if (metrics.value("firstFrameMs").toDouble() <= 0) return;
            const auto presentedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - launched).count();
            const QJsonObject result{{"hours", hours}, {"layer", layer},
                                     {"entries", metrics.value("entries").toDouble()},
                                     {"load_ms", metrics.value("loadMs").toDouble()},
                                     {"decode_ms", metrics.value("decodeMs").toDouble()},
                                     {"first_draw_submit_ms", metrics.value("firstFrameMs").toDouble()},
                                     {"launch_to_first_frame_ms", presentedMs}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
            app.quit();
        }, Qt::QueuedConnection);
        QTimer::singleShot(30'000, &app, [&app] { app.exit(2); });
    }
    return app.exec();
}
