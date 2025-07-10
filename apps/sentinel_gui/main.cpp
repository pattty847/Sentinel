#include "mainwindow_gpu.h"
#include <QApplication>
#include <QDebug>
#include <iostream>
#include "tradedata.h"
#include <QMetaType>
#include "testdataplayer.h"

int main(int argc, char *argv[])
{
    std::cout << "🚀 [Sentinel GPU Trading Terminal Starting...]" << std::endl;
    QApplication app(argc, argv);
    
    // Register custom types for signal/slot connections
    qRegisterMetaType<Trade>();
    qRegisterMetaType<OrderBook>();
    
    // Check for --test-mode flag
    bool testMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test-mode") {
            testMode = true;
            break;
        }
    }

    // 🔥 CREATE GPU-POWERED MAIN WINDOW
    MainWindowGPU window(testMode);
    window.show();
        
    return app.exec();
}