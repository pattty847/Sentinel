#include "MainWindowGpu.h"
#include <QApplication>
#include <QDebug>
#include <iostream>
#include "TradeData.h"
#include <QMetaType>
#include <QQmlEngine>
#include "UnifiedGridRenderer.h"
#include "CoordinateSystem.h"
#include "models/AxisModel.hpp"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"

int main(int argc, char *argv[])
{
    std::cout << "🚀 [Sentinel GPU Trading Terminal Starting...]" << std::endl;
    QApplication app(argc, argv);
    
    // Register custom types for signal/slot connections
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    
    // 🎯 PHASE 5: Pure grid-only QML component registration
    std::cout << "🎯 Registering pure grid-only QML components..." << std::endl;
    
    qmlRegisterType<UnifiedGridRenderer>("Sentinel.Charts", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel.Charts", 1, 0, "CoordinateSystem");
    
    // Register axis models for clean QML grid line calculations
    qmlRegisterType<TimeAxisModel>("Sentinel.Charts", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel.Charts", 1, 0, "PriceAxisModel");
    
    std::cout << "✅ Pure grid-only mode: Legacy components permanently removed" << std::endl;
    
    // 🔥 CREATE GPU-POWERED MAIN WINDOW
    MainWindowGPU window;
    window.show();
    
    qDebug() << "✅ GPU Trading Terminal ready for 144Hz action!";
    
    return app.exec();
}