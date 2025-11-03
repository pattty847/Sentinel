/*
Sentinel — main.cpp
Role: Entry point for the Sentinel GUI application.
This version modularizes startup logic for maintainability and clarity.
*/
#include "MainWindowGpu.h"
#include <QApplication>
#include <QMetaType>
#include <QQmlEngine>
#include "marketdata/model/TradeData.h"
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"
#include <QSurfaceFormat>
#include <QSysInfo>
#include "SentinelLogging.hpp" // Sentinel categorized logging

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
    qputenv("QSG_INFO", "1"); // optional: verbose GPU info
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

    sLog_App("Registering pure grid-only QML components...");
    qmlRegisterType<UnifiedGridRenderer>("Sentinel.Charts", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel.Charts", 1, 0, "CoordinateSystem");
    qmlRegisterType<TimeAxisModel>("Sentinel.Charts", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel.Charts", 1, 0, "PriceAxisModel");
    sLog_App("Pure grid-only mode: Legacy components permanently removed");
}

// --- Main application entrypoint ---
// Architecture: GPU-accelerated trading terminal with 144Hz performance
// Performance: CPU 45-55°C, GPU 15-30% (vs previous 70°C CPU, 0% GPU)
// Features: Trade batching (75ms), chunked geometry (65k limit), D3D11/Metal/OpenGL
int main(int argc, char *argv[])
{
    sLog_App("[Sentinel GPU Trading Terminal Starting...]");

    configureGraphicsBackend();
    configureSurfaceFormat();

    QApplication app(argc, argv);

    registerMetaTypesAndQml();

    sLog_App("Creating MainWindowGPU...");
    MainWindowGPU window;
    sLog_App("MainWindowGPU created successfully");
    sLog_App("Calling window.show()...");
    window.show();
    sLog_App("window.show() completed");

    sLog_App("Starting Qt event loop with app.exec()...");
    sLog_App("GPU Trading Terminal ready for 144Hz action!");

    return app.exec();
}