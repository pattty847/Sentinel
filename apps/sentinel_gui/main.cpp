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
#include "render/LabTextItem.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include <QSurfaceFormat>
#include <QSysInfo>
#include "SentinelLogging.hpp"
#include "themes/ThemeManager.hpp"
#include "themes/FontManager.hpp"
#include "ConfigLoader.hpp"
#include "config/GuiConfigStore.hpp"
#include <QResource>
#include <QCoreApplication>
// --- Hardware backend/environment setup ---
void configureGraphicsBackend() {
    #ifdef Q_OS_WIN
        // Force OpenGL on Windows for heatmap texture uploads.
        qputenv("QSG_RHI_BACKEND", "opengl");
    #elif defined(Q_OS_MACOS)
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
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
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

    // Register Sentinel.Charts module types for LabView
    qmlRegisterModule("Sentinel.Charts", 1, 0);
    qmlRegisterType<LabTextItem>("Sentinel.Charts", 1, 0, "LabTextItem");
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    qmlRegisterType<CandlestickOverlayItem>("Sentinel.Charts", 1, 0, "CandlestickOverlayItem");
}

// --- Main application entrypoint ---
int main(int argc, char *argv[])
{
    ClientConfig clientConfig;
    ConfigLoader::loadClientConfig("config/client_config.yaml", &clientConfig);
    ConfigLoader::loadClientConfig("config/.client_config.yaml", &clientConfig);
    GuiConfigStore::instance().setClientConfig(clientConfig);

    configureGraphicsBackend();
    configureSurfaceFormat();

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
        sLog_Error("Failed to apply default theme");
    }

    FontManager::instance().initialize(&app);

    registerMetaTypesAndQml();
    qRegisterMetaType<ServerConfig>("ServerConfig");
    qRegisterMetaType<ClientConfig>("ClientConfig");

    MainWindowGPU window;
    window.show();

    return app.exec();
}
