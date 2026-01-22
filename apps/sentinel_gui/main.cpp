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
#include <QSurfaceFormat>
#include <QSysInfo>
#include "SentinelLogging.hpp"
#include "themes/ThemeManager.hpp"

// --- Hardware backend/environment setup ---
void configureGraphicsBackend() {
    #ifdef Q_OS_WIN
        qputenv("QSG_RHI_BACKEND", "d3d11");
    #elif defined(Q_OS_MACOS)
        qputenv("QSG_RHI_BACKEND", "metal");
    #else
        qputenv("QSG_RHI_BACKEND", "opengl");
    #endif
    qputenv("QSG_RENDER_LOOP", "threaded");
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
}

// --- Main application entrypoint ---
int main(int argc, char *argv[])
{
    configureGraphicsBackend();
    configureSurfaceFormat();

    QApplication app(argc, argv);

    // Initialize and apply theme
    ThemeManager& themeManager = ThemeManager::instance();
    themeManager.initializeDefaults();

    if (!themeManager.applyTheme("dark", &app)) {
        sLog_Error("Failed to apply default theme");
    }

    registerMetaTypesAndQml();

    MainWindowGPU window;
    window.show();

    return app.exec();
}