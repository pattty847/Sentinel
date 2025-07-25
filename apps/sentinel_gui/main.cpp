#include "mainwindow_gpu.h"
#include <QApplication>
#include <QDebug>
#include <iostream>
#include "tradedata.h"
#include <QMetaType>
#include <QQmlEngine>
#include "UnifiedGridRenderer.h"
#include "GridIntegrationAdapter.h"
#include "CoordinateSystem.h"

int main(int argc, char *argv[])
{
    std::cout << "🚀 [Sentinel GPU Trading Terminal Starting...]" << std::endl;
    QApplication app(argc, argv);
    
    // Register custom types for signal/slot connections
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    
    // 🎯 PHASE 5: Pure grid-only QML component registration
    std::cout << "🎯 Registering pure grid-only QML components..." << std::endl;
    
    qmlRegisterType<GridIntegrationAdapter>("Sentinel.Charts", 1, 0, "GridIntegrationAdapter");
    qmlRegisterType<UnifiedGridRenderer>("Sentinel.Charts", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<CoordinateSystem>("Sentinel.Charts", 1, 0, "CoordinateSystem");
    
    std::cout << "✅ Pure grid-only mode: Legacy components permanently removed" << std::endl;
    
    // 🔥 CREATE GPU-POWERED MAIN WINDOW
    MainWindowGPU window;
    window.show();
    
    qDebug() << "✅ GPU Trading Terminal ready for 144Hz action!";
    
    return app.exec();
}