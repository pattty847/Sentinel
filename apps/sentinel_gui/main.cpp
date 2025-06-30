#include "mainwindow_gpu.h"
#include <QApplication>
#include "Log.hpp"
#include <iostream>
#include "tradedata.h"
#include <QMetaType>

int main(int argc, char *argv[])
{
    std::cout << "🚀 [Sentinel GPU Trading Terminal Starting...]" << std::endl;
    QApplication app(argc, argv);
    
    // Register custom types for signal/slot connections
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    
    // 🔥 CREATE GPU-POWERED MAIN WINDOW
    MainWindowGPU window;
    window.show();
    
    LOG_I("App", "✅ GPU Trading Terminal ready for 144Hz action!");
    
    return app.exec();
}