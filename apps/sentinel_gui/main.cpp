/*
Sentinel — main.cpp
Role: Entry point for the Sentinel GUI application.
This version modularizes startup logic for maintainability and clarity.
*/
#include "MainWindowGpu.h"
#include <QApplication>
#include <QMetaType>
#include <QQmlEngine>
#include <QtQml/qqml.h>
#include <QByteArray> // for qputenv / qgetenv on all platforms
#include "marketdata/model/TradeData.h"
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include "render/AlgoOverlayRenderer.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "render/PaperTradeOverlayRenderer.hpp"
#include <QSurfaceFormat>
#include <QSysInfo>
#include "SentinelLogging.hpp"
#include "SentinelLogSink.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/FontManager.hpp"
#include "ConfigLoader.hpp"
#include "config/AgentHostMode.hpp"
#include "config/GuiConfigStore.hpp"
#include <QFileInfo>
#include <cstdio>
#include <QQuickWindow>
#include <QResource>
#include <QCoreApplication>
#include <QDir>
// --- Hardware backend/environment setup ---
void configureGraphicsBackend() {
    #ifdef Q_OS_WIN
        // Direct3D 12 on Windows: the GPU heatmap needs storage buffers in the
        // fragment stage and more UAVs than D3D11 offers. QSG_RHI_BACKEND still
        // overrides it (setGraphicsApi would win over the variable, so skip it).
        if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
    #elif defined(Q_OS_MACOS)
        // Default to Metal on macOS; non-OpenGL upload fallback handles heatmap/footprint updates.
        qputenv("QSG_RHI_BACKEND", "metal"); 
    #else
        qputenv("QSG_RHI_BACKEND", "opengl");
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    #endif
    if (qEnvironmentVariableIsSet("SENTINEL_QSG_RENDER_LOOP")) {
        const QByteArray loop = qgetenv("SENTINEL_QSG_RENDER_LOOP");
        if (!loop.isEmpty()) {
            qputenv("QSG_RENDER_LOOP", loop);
        }
    } else if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", "threaded");
    }
}

// --- Surface format configuration ---
void configureSurfaceFormat() {
    QSurfaceFormat fmt;
    const QByteArray backend = qgetenv("QSG_RHI_BACKEND").toLower();
    if (backend == "opengl") {
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
    }
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(0); // disable vsync for realtime charts
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);
}

// --- Qt metatype and QML component registration ---
void registerMetaTypesAndQml() {
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    qRegisterMetaType<std::shared_ptr<const OrderBook>>("std::shared_ptr<const OrderBook>");

    qmlRegisterModule("Sentinel", 1, 0);
    qmlRegisterType<UnifiedGridRenderer>("Sentinel", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel", 1, 0, "CoordinateSystem");
    qmlRegisterType<TimeAxisModel>("Sentinel", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel", 1, 0, "PriceAxisModel");
    qmlRegisterType<AlgoOverlayRenderer>("Sentinel", 1, 0, "AlgoOverlayRenderer");
    qmlRegisterType<PaperTradeOverlayModel>("Sentinel", 1, 0, "PaperTradeOverlayModel");
    qmlRegisterType<PaperTradeOverlayRenderer>("Sentinel", 1, 0, "PaperTradeOverlayRenderer");

    // Shared candle QML types used by production charts.
    qmlRegisterModule("Sentinel.Charts", 1, 0);
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    qmlRegisterType<CandlestickOverlayItem>("Sentinel.Charts", 1, 0, "CandlestickOverlayItem");
}

// --- Main application entrypoint ---
int main(int argc, char *argv[])
{
    sentinel::logging::installLogSink("sentinel-gui", argc, argv);

    ClientConfig clientConfig;
    if (!ConfigLoader::loadClientConfig("config/client_config.yaml", &clientConfig)) {
        sLog_Warning("Client config not loaded, using defaults: path=config/client_config.yaml"
                     << " cwd=" << QDir::currentPath());
    }
    ConfigLoader::loadClientConfig("config/.client_config.yaml", &clientConfig);
    // Process-only overrides:
    //   --heatmap-renderer gpu          accepted and ignored: gpu is the only renderer (S8a)
    //   --api-port N                    Agent API port instead of gui.api_port
    //   --no-screener                   no screener_server.py child (it owns port 17200 and
    //                                   kills its holder at start, so a second process must not)
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--no-screener") clientConfig.gui.startScreener = false;
    QString agentDockProfileFile;
    for (int i = 1; i + 1 < argc; ++i)
        if (QByteArray(argv[i]) == "--agent-host-profile") agentDockProfileFile = QString::fromLocal8Bit(argv[i + 1]);
    for (int i = 1; i + 1 < argc; ++i) {
        const QByteArray flag(argv[i]), value(argv[i + 1]);
        if (flag == "--heatmap-renderer") {
            if (value != "gpu")
                sLog_Warning("Ignored --heatmap-renderer " << value
                             << ": the legacy heatmap renderer was removed (S8a); the chart draws with gpu");
        } else if (flag == "--agent-host") {
            // Launched by scripts/dev/gui-host.py for sandboxed agents (see AgentHostMode.hpp).
            // Must run before any QSettings use. The checkout (cwd) and the build tree are
            // agent-writable, so the session directory may not live in them.
            QString why;
            const QString binDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
            if (!AgentHostMode::activate(QString::fromLocal8Bit(value), {QDir::currentPath(), binDir}, &why,
                                         agentDockProfileFile)) {
                sLog_Error("--agent-host refused: " << why);
                fprintf(stderr, "sentinel-gui: --agent-host refused: %s\n", qPrintable(why));
                return 2;
            }
            sLog_App("agent-host mode: dir=" << value);
        } else if (flag == "--agent-host-symbols") { // comma list; with --agent-host, the only symbols it may switch to
            AgentHostMode::setSymbolAllowlist(QString::fromLocal8Bit(value).split(',', Qt::SkipEmptyParts));
        } else if (flag == "--api-port") {
            bool ok = false;
            const int port = value.toInt(&ok);
            if (ok && port > 0 && port <= 65535) clientConfig.gui.apiPort = port;
            else sLog_Warning("Ignored --api-port " << value);
        }
    }
    GuiConfigStore::instance().setClientConfig(clientConfig);

    configureGraphicsBackend();
    configureSurfaceFormat();
    sLog_App("GUI startup: server=" << clientConfig.server.host << ":" << clientConfig.server.port
             << " rhiBackend=" << (!qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") ? qgetenv("QSG_RHI_BACKEND")
                                   : QQuickWindow::graphicsApi() == QSGRendererInterface::Direct3D12 ? QByteArray("d3d12")
                                                                                                     : QByteArray("qt-default"))
             << " renderLoop=" << qgetenv("QSG_RENDER_LOOP"));

    const int disableCompress = qEnvironmentVariableIntValue("SENTINEL_DISABLE_HF_EVENT_COMPRESSION");
    if (disableCompress == 1) {
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    }

    QApplication app(argc, argv);

    // Register resources embedded in the static GUI library.
    Q_INIT_RESOURCE(sentinel_svg_resources);
    Q_INIT_RESOURCE(sentinel_ui_fonts);

    // Initialize and apply theme
    ThemeManager& themeManager = ThemeManager::instance();
    themeManager.initializeDefaults();

    if (!themeManager.applyTheme("dark", &app)) {
        sLog_Warning("Failed to apply default theme: theme=dark");
    }

    FontManager::instance().initialize(&app);

    registerMetaTypesAndQml();
    qRegisterMetaType<ServerConfig>("ServerConfig");
    qRegisterMetaType<ClientConfig>("ClientConfig");

    MainWindowGPU window;
    window.show();

    return app.exec();
}
