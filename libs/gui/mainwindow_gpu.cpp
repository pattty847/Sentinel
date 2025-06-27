#include "mainwindow_gpu.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "gpuchartwidget.h"
#include "heatmapinstanced.h"
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimer>

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {
    qDebug() << "🚀 CREATING GPU TRADING TERMINAL!";
    
    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    
    qDebug() << "✅ GPU MainWindow ready for 144Hz trading!";
}

void MainWindowGPU::setupUI() {
    // 🔥 CREATE GPU CHART (QML) - Load from file system first
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    
    // Try file system path first to bypass resource issues
    QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
    qDebug() << "🔍 Trying QML path:" << qmlPath;
    
    if (QFile::exists(qmlPath)) {
        m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        qDebug() << "✅ QML loaded from file system!";
    } else {
        qDebug() << "❌ QML file not found, trying resource path...";
        m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    // Check if QML loaded successfully
    if (m_gpuChart->status() == QQuickWidget::Error) {
        qCritical() << "🚨 QML FAILED TO LOAD!";
        qCritical() << "QML Errors:" << m_gpuChart->errors();
    }
    
    // Set default symbol in QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", "BTC-USD");
    
    // Control panel
    m_cvdLabel = new QLabel("CVD: N/A", this);
    m_statusLabel = new QLabel("🔴 Disconnected", this);
    m_symbolInput = new QLineEdit("BTC-USD", this);
    m_subscribeButton = new QPushButton("🚀 Subscribe", this);
    
    // 🔥 PHASE 1: STRESS TEST CONTROLS
    QPushButton* stressTestButton = new QPushButton("🔥 1M Point VBO Test", this);
    stressTestButton->setStyleSheet("QPushButton { background-color: #ff4444; color: white; padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    connect(stressTestButton, &QPushButton::clicked, [this]() {
        qDebug() << "🚀 LAUNCHING 1M POINT VBO STRESS TEST!";
        QQmlContext* context = m_gpuChart->rootContext();
        context->setContextProperty("stressTestMode", true);
        
        // Reload QML to trigger stress test
        QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
        if (QFile::exists(qmlPath)) {
            m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        } else {
            m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
        }
        
        // 🔥 CRITICAL FIX: Re-establish connections after QML reload
        QTimer::singleShot(100, [this]() {  // Small delay to ensure QML is loaded
            connectToGPUChart();
        });
        
        qDebug() << "🔥 VBO STRESS TEST ACTIVATED!";
    });
    
    // Styling
    m_cvdLabel->setStyleSheet("QLabel { color: white; font-size: 16px; font-weight: bold; }");
    m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
    m_symbolInput->setStyleSheet("QLineEdit { padding: 8px; font-size: 14px; }");
    m_subscribeButton->setStyleSheet("QPushButton { padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    // Layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // GPU chart takes most space
    mainLayout->addWidget(m_gpuChart, 1);
    
    // Control panel at bottom
    QGroupBox* controlGroup = new QGroupBox("🎯 Trading Controls");
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->addWidget(new QLabel("Symbol:"));
    controlLayout->addWidget(m_symbolInput);
    controlLayout->addWidget(m_subscribeButton);
    controlLayout->addWidget(stressTestButton);  // 🔥 STRESS TEST BUTTON
    controlLayout->addStretch();
    controlLayout->addWidget(m_cvdLabel);
    controlLayout->addWidget(m_statusLabel);
    controlGroup->setLayout(controlLayout);
    
    mainLayout->addWidget(controlGroup);
    setLayout(mainLayout);
}

void MainWindowGPU::setupConnections() {
    // Create data controllers (keep existing proven pipeline)
    m_streamController = new StreamController(this);
    m_statsController = new StatisticsController(this);
    
    // UI connections
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    
    // Data pipeline connections
    connect(m_streamController, &StreamController::connected, this, [this]() {
        m_statusLabel->setText("🟢 Connected");
        m_statusLabel->setStyleSheet("QLabel { color: green; font-size: 14px; }");
        m_subscribeButton->setText("🔗 Connected");
        m_subscribeButton->setEnabled(false);
    });
    
    connect(m_streamController, &StreamController::disconnected, this, [this]() {
        m_statusLabel->setText("🔴 Disconnected");
        m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
        m_subscribeButton->setText("🚀 Subscribe");
        m_subscribeButton->setEnabled(true);
    });
    
    // Stats pipeline
    connect(m_statsController, &StatisticsController::cvdUpdated, 
            this, &MainWindowGPU::onCVDUpdated);
    
    // 🔥 CRITICAL FIX: Establish GPU connections
    connectToGPUChart();
    
    qDebug() << "✅ GPU MainWindow connections established";
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        qWarning() << "❌ Empty symbol input";
        return;
    }
    
    qDebug() << "🚀 Subscribing to GPU chart:" << symbol;
    
    // Update QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", symbol);
    
    // Start data stream
    std::vector<std::string> symbols = {symbol.toStdString()};
    m_streamController->start(symbols);
    
    qDebug() << "✅ GPU subscription started for" << symbol;
}

void MainWindowGPU::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

// 🔥 CRITICAL FIX: Helper method to establish GPU connections
void MainWindowGPU::connectToGPUChart() {
    // Get the GPU chart widget from QML
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (qmlRoot) {
        GPUChartWidget* gpuChart = nullptr;
        
        // 🔥 STRATEGY 1: Find by explicit objectName
        QQuickItem* gpuChartItem = qmlRoot->findChild<QQuickItem*>("gpuChart");
        gpuChart = qobject_cast<GPUChartWidget*>(gpuChartItem);
        
        // 🔥 STRATEGY 2: If objectName lookup fails, find by type
        if (!gpuChart) {
            qDebug() << "🔍 objectName lookup failed, trying type lookup...";
            gpuChart = qmlRoot->findChild<GPUChartWidget*>();
        }
        
        if (gpuChart) {
            // 🔥 CRITICAL: Disconnect any existing connections to avoid duplicates
            disconnect(m_streamController, &StreamController::tradeReceived,
                      gpuChart, &GPUChartWidget::onTradeReceived);
                      
            // 🔥 REAL-TIME DATA CONNECTION! (Qt::QueuedConnection for thread safety)
            connect(m_streamController, &StreamController::tradeReceived,
                    gpuChart, &GPUChartWidget::onTradeReceived,
                    Qt::QueuedConnection); // CRITICAL: Async connection for thread safety!
            
            qDebug() << "🔥 GPU CHART CONNECTED TO REAL-TIME TRADE DATA!";
            
            // 🔥 GEMINI UNIFICATION: Connect chart view to heatmap
            QQuickItem* heatmapItem = qmlRoot->findChild<QQuickItem*>("heatmapLayer");
            HeatMapInstanced* heatmapLayer = qobject_cast<HeatMapInstanced*>(heatmapItem);
            
            if (heatmapLayer) {
                // 🔥 THE CRITICAL BRIDGE: Chart view coordinates to heatmap
                connect(gpuChart, &GPUChartWidget::viewChanged,
                        heatmapLayer, &HeatMapInstanced::setTimeWindow,
                        Qt::QueuedConnection);
                
                qDebug() << "✅🔥 VIEW COORDINATION ESTABLISHED: Chart view is now wired to Heatmap.";
            } else {
                qWarning() << "⚠️ HeatmapInstanced not found - coordinate unification failed";
            }
        } else {
            qWarning() << "⚠️ GPUChartWidget not found using any strategy";
            qDebug() << "🔍 Available children:";
            for (QObject* child : qmlRoot->children()) {
                qDebug() << "  -" << child->objectName() << child->metaObject()->className();
            }
        }
        
        // 🔥 PHASE 2: Connect order book data to HeatmapInstanced - CRITICAL FIX!
        HeatMapInstanced* heatmapLayer = qmlRoot->findChild<HeatMapInstanced*>("heatmapLayer");
        if (heatmapLayer) {
            connect(m_streamController, &StreamController::orderBookUpdated,
                    heatmapLayer, &HeatMapInstanced::onOrderBookUpdated,
                    Qt::QueuedConnection); // CRITICAL: Async connection for thread safety!
            
            qDebug() << "🔥 HEATMAP CONNECTED TO REAL-TIME ORDER BOOK DATA!";
        } else {
            qWarning() << "⚠️ HeatMapInstanced (heatmapLayer) not found in QML root - CHECK OBJECTNAME!";
        }
    } else {
        qWarning() << "⚠️ QML root object not found";
    }
} 