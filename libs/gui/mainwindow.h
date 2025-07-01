#pragma once

#include <QWidget>

// 📦 DEPRECATED - Use MainWindowGPU instead
// This is just a stub so old mainwindow.cpp compiles
class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr) : QWidget(parent) {}
private slots:
    void onSubscribe() {}
    void onCVDUpdated(double) {}
    void onAlert(const QString&) {}
}; 