#include "lab/Bench.hpp"
#include "lab/LabItem.hpp"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QtQml/qqml.h>

int main(int argc, char **argv) {
    bool benchmark = false;
    for (int i = 1; i < argc; ++i) benchmark |= QByteArray(argv[i]) == "--bench";
    if (benchmark) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sentinel-lab"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"bench", "Run 200 headless Metal bin passes and emit JSON"});
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
    return app.exec();
}
