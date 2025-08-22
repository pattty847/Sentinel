/*
Sentinel — main.cpp
Role: Main entry point for the Sentinel GUI application.
Inputs/Outputs: Standard command-line arguments -> application exit code.
Threading: Runs exclusively on the main application/GUI thread.
Performance: One-time application startup logic; not on a critical performance path.
Integration: Top-level executable; launches MainWindowGPU and registers C++ types with QML.
Observability: Logs key startup milestones to stdout/qDebug.
Related: MainWindowGpu.h, UnifiedGridRenderer.h, CoordinateSystem.h, TimeAxisModel.hpp.
Assumptions: Executed as the primary entry point of a Qt application.
*/
#include "MainWindowGpu.h"
#include <QApplication>
#include <QDebug>
#include <iostream>
#include "TradeData.h"
#include <QMetaType>
#include <QQmlEngine>
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"

int main(int argc, char *argv[])
{
    std::cout << "🚀 [Sentinel GPU Trading Terminal Starting...]" << std::endl;
    QApplication app(argc, argv);
    
    // PHASE 1.3: Consolidated type registrations (no duplicates)
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    qRegisterMetaType<std::shared_ptr<const OrderBook>>("std::shared_ptr<const OrderBook>");
    
    // PHASE 2.1: Dense-only signal uses QString productId (no complex type needed)
    
    // 🎯 PHASE 5: Pure grid-only QML component registration
    std::cout << "🎯 Registering pure grid-only QML components..." << std::endl;
    
    qmlRegisterType<UnifiedGridRenderer>("Sentinel.Charts", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel.Charts", 1, 0, "CoordinateSystem");
    
    // Register axis models for clean QML grid line calculations
    qmlRegisterType<TimeAxisModel>("Sentinel.Charts", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel.Charts", 1, 0, "PriceAxisModel");
    
    std::cout << "✅ Pure grid-only mode: Legacy components permanently removed" << std::endl;
    
    // 🔥 CREATE GPU-POWERED MAIN WINDOW
    std::cout << "🔧 Creating MainWindowGPU..." << std::endl;
    MainWindowGPU window;
    std::cout << "✅ MainWindowGPU created successfully" << std::endl;
    
    std::cout << "🔧 Calling window.show()..." << std::endl;
    window.show();
    std::cout << "✅ window.show() completed" << std::endl;
    
    std::cout << "🔧 Starting Qt event loop with app.exec()..." << std::endl;
    qDebug() << "✅ GPU Trading Terminal ready for 144Hz action!";
    
    return app.exec();
}