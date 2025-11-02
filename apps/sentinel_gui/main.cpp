/*
Sentinel — main.cpp
Role: Entry point for the Sentinel GUI application.
This version modularizes startup logic for maintainability and clarity.
*/
#include "MainWindowGpu.h"
#include <QApplication>
#include <QDebug>
#include <iostream>
#include "marketdata/model/TradeData.h"
#include <QMetaType>
#include <QQmlEngine>
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"
#include <QSurfaceFormat>
#include <QSysInfo>

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

    std::cout << "Registering pure grid-only QML components..." << std::endl;
    qmlRegisterType<UnifiedGridRenderer>("Sentinel.Charts", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel.Charts", 1, 0, "CoordinateSystem");
    qmlRegisterType<TimeAxisModel>("Sentinel.Charts", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel.Charts", 1, 0, "PriceAxisModel");
    std::cout << "Pure grid-only mode: Legacy components permanently removed" << std::endl;
}

// --- Main window creation ---
MainWindowGPU* createAndShowMainWindow() {
    std::cout << "Creating MainWindowGPU..." << std::endl;
    auto* window = new MainWindowGPU();
    std::cout << "MainWindowGPU created successfully" << std::endl;
    std::cout << "Calling window.show()..." << std::endl;
    window->show();
    std::cout << "window.show() completed" << std::endl;
    return window;
}

// --- Main application entrypoint ---
int main(int argc, char *argv[])
{
    std::cout << "[Sentinel GPU Trading Terminal Starting...]" << std::endl;

    configureGraphicsBackend();
    configureSurfaceFormat();

    QApplication app(argc, argv);

    registerMetaTypesAndQml();
    createAndShowMainWindow();

    std::cout << "Starting Qt event loop with app.exec()..." << std::endl;
    qDebug() << "GPU Trading Terminal ready for 144Hz action!";

    return app.exec();
}