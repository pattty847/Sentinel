#include "mainwindow.h"
#include <QApplication>
#include <iostream>
#include "tradedata.h"
#include <QMetaType>

int main(int argc, char *argv[])
{
    std::cout << "[Sentinel Stream Test Starting...]" << std::endl;
    QApplication a(argc, argv);
    
    // Register custom types for signal/slot connections across threads
    qRegisterMetaType<OrderBook>();

    MainWindow w;
    w.show();
    return a.exec();
}