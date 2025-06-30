#include "mainwindow_gpu.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "gpuchartwidget.h"
#include "heatmapinstanced.h"
#include "candlestickbatched.h"
#include "chartmodecontroller.h"
#include "gpudataadapter.h"
#include <QQmlContext>
#include "Log.hpp"
#include <QDir>
#include <QFile>
#include <QTimer>

static constexpr auto CAT = "MainWindowGPU";

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {
    LOG_I(CAT, "🚀 CREATING GPU TRADING TERMINAL!");
    
    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    
    // Automatically start the subscription process once the event loop begins.
    // This ensures the window is fully constructed and shown before we start streaming.
    QTimer::singleShot(0, this, &MainWindowGPU::onSubscribe);
    
    LOG_I(CAT, "✅ GPU MainWindow ready for 144Hz trading!");
}

void MainWindowGPU::setupUI() {
    // 🔥 CREATE GPU CHART (QML) - Load from file system first
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    
    // Try file system path first to bypass resource issues
    QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
    LOG_D(CAT, "🔍 Trying QML path: {}", qmlPath.toStdString());
    
    if (QFile::exists(qmlPath)) {
        m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        LOG_I(CAT, "✅ QML loaded from file system!");
    } else {
        LOG_W(CAT, "❌ QML file not found, trying resource path...");
        m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    // Check if QML loaded successfully
    if (m_gpuChart->status() == QQuickWidget::Error) {
        LOG_E(CAT, "🚨 QML FAILED TO LOAD!");
        LOG_E(CAT, "QML Errors: {}", m_gpuChart->errors());
    }
    
    // Set default symbol in QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", "BTC-USD");
    m_modeController = new ChartModeController(this);
    context->setContextProperty("chartModeController", m_modeController);
    
    // Control panel
    m_cvdLabel = new QLabel("CVD: N/A", this);
    m_statusLabel = new QLabel("🔴 Disconnected", this);
    m_symbolInput = new QLineEdit("BTC-USD", this);
    m_subscribeButton = new QPushButton("🚀 Subscribe", this);
    
    // 🔥 PHASE 1: STRESS TEST CONTROLS
    QPushButton* stressTestButton = new QPushButton("🔥 1M Point VBO Test", this);
    stressTestButton->setStyleSheet("QPushButton { background-color: #ff4444; color: white; padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    connect(stressTestButton, &QPushButton::clicked, [this]() {
    LOG_I(CAT, "🚀 LAUNCHING 1M POINT VBO STRESS TEST!");
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
        
        LOG_I(CAT, "🔥 VBO STRESS TEST ACTIVATED!");
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
    m_gpuAdapter = new GPUDataAdapter(this);
    m_streamController->setGPUAdapter(m_gpuAdapter);
    
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
    
    LOG_I(CAT, "✅ GPU MainWindow connections established");
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        LOG_W(CAT, "❌ Empty symbol input");
        return;
    }
    
    LOG_I(CAT, "🚀 Subscribing to GPU chart: {}", symbol.toStdString());
    
    // Update QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", symbol);
    
    // Start data stream
    std::vector<std::string> symbols = {symbol.toStdString()};
    m_streamController->start(symbols);
    
    LOG_I(CAT, "✅ GPU subscription started for {}", symbol.toStdString());
}

void MainWindowGPU::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

// 🔥 CRITICAL FIX: Helper method to establish GPU connections
void MainWindowGPU::connectToGPUChart() {
    LOG_D(CAT, "🔍 ATTEMPTING TO CONNECT TO GPU CHART...");
    
    // Get the GPU chart widget from QML
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        LOG_W(CAT, "❌ QML root object not found - retrying in 100ms...");
        // Retry after QML loads
        QTimer::singleShot(100, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    LOG_D(CAT, "✅ QML root found: {}", static_cast<void*>(qmlRoot));
    LOG_D(CAT, "🔍 QML root children:");
    for (QObject* child : qmlRoot->children()) {
        LOG_D(CAT, "  - {} {}", child->objectName().toStdString(), child->metaObject()->className());
    }
    
    GPUChartWidget* gpuChart = nullptr;
    
    // 🔥 STRATEGY 1: Find by explicit objectName
    QQuickItem* gpuChartItem = qmlRoot->findChild<QQuickItem*>("gpuChart");
    LOG_D(CAT, "🔍 objectName lookup for 'gpuChart': {}", static_cast<void*>(gpuChartItem));
    
    if (gpuChartItem) {
        gpuChart = qobject_cast<GPUChartWidget*>(gpuChartItem);
        LOG_D(CAT, "🔍 qobject_cast to GPUChartWidget: {}", static_cast<void*>(gpuChart));
    }
    
    // 🔥 STRATEGY 2: If objectName lookup fails, find by type
    if (!gpuChart) {
        LOG_D(CAT, "🔍 objectName lookup failed, trying type lookup...");
        gpuChart = qmlRoot->findChild<GPUChartWidget*>();
        LOG_D(CAT, "🔍 Type lookup result: {}", static_cast<void*>(gpuChart));
    }
    
    if (gpuChart) {
        // 🔥 CRITICAL: Disconnect any existing connections to avoid duplicates
        disconnect(m_streamController, &StreamController::tradeReceived,
                  gpuChart, &GPUChartWidget::onTradeReceived);
                  
        // 🔥 REAL-TIME DATA CONNECTION! (Qt::QueuedConnection for thread safety)
        bool connectionSuccess = connect(m_streamController, &StreamController::tradeReceived,
                                        gpuChart, &GPUChartWidget::onTradeReceived,
                                        Qt::QueuedConnection); // CRITICAL: Async connection for thread safety!
                                        
        LOG_D(CAT, "🔗 Qt Signal Connection Result: {}", (connectionSuccess ? "SUCCESS" : "FAILED"));
        LOG_D(CAT, "🎯 Connected StreamController: {} to GPUChart: {}", static_cast<void*>(m_streamController), static_cast<void*>(gpuChart));
        
        LOG_I(CAT, "🔥 GPU CHART CONNECTED TO REAL-TIME TRADE DATA!");
        
        // 🔥 GEMINI UNIFICATION: Connect chart view to heatmap
        QQuickItem* heatmapItem = qmlRoot->findChild<QQuickItem*>("heatmapLayer");
        HeatMapInstanced* heatmapLayer = qobject_cast<HeatMapInstanced*>(heatmapItem);
        
        if (heatmapLayer) {
            // 🔥 THE CRITICAL BRIDGE: Chart view coordinates to heatmap
            connect(gpuChart, &GPUChartWidget::viewChanged,
                    heatmapLayer, &HeatMapInstanced::setTimeWindow,
                    Qt::QueuedConnection);
            
            LOG_I(CAT, "✅🔥 VIEW COORDINATION ESTABLISHED: Chart view is now wired to Heatmap.");
        } else {
            LOG_W(CAT, "⚠️ HeatmapInstanced not found - coordinate unification failed");
        }
        
        // 🕯️ PHASE 5: CONNECT CANDLESTICK CHART
        LOG_D(CAT, "🔍 Looking for candlestick renderer...");
        QQuickItem* candleChartItem = qmlRoot->findChild<QQuickItem*>("candlestickRenderer");
        LOG_D(CAT, "🔍 Found candlestickRenderer item: {}", static_cast<void*>(candleChartItem));
        
        CandlestickBatched* candleChart = qobject_cast<CandlestickBatched*>(candleChartItem);
        LOG_D(CAT, "🔍 Cast to CandlestickBatched: {}", static_cast<void*>(candleChart));
        
        if (candleChart) {
            bool tradeConnection = connect(m_gpuAdapter, &GPUDataAdapter::candlesReady,
                                          candleChart, &CandlestickBatched::onCandlesReady,
                                          Qt::QueuedConnection);
            
            // 🔥 COORDINATE SYNCHRONIZATION: GPUChart drives candle coordinates
            bool coordConnection = connect(gpuChart, &GPUChartWidget::viewChanged,
                                          candleChart, &CandlestickBatched::onViewChanged,
                                          Qt::QueuedConnection);
            
            LOG_D(CAT, "🕯️ CANDLESTICK CONNECTIONS: Candle stream:{} Coordinates:{}",
                  (tradeConnection ? "SUCCESS" : "FAILED"), (coordConnection ? "SUCCESS" : "FAILED"));
            
            if (tradeConnection && coordConnection) {
                LOG_I(CAT, "✅ CANDLESTICK CHART FULLY CONNECTED TO REAL-TIME DATA!");
            }
        } else {
            LOG_W(CAT, "❌ CandlestickBatched (candlestickRenderer) not found - candle integration failed");
            
            // List all available QML children for debugging
            LOG_D(CAT, "🔍 Available QML children with objectNames:");
            for (QObject* child : qmlRoot->findChildren<QObject*>()) {
                QString objName = child->objectName();
                if (!objName.isEmpty()) {
                    LOG_D(CAT, "  - {} : {}", objName.toStdString(), child->metaObject()->className());
                }
            }
        }
    } else {
        LOG_W(CAT, "❌ GPUChartWidget NOT FOUND - RETRYING IN 200ms...");
        // Retry after a longer delay
        QTimer::singleShot(200, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    // 🔥 PHASE 2: Connect order book data to HeatmapInstanced - CRITICAL FIX!
    HeatMapInstanced* heatmapLayer = qmlRoot->findChild<HeatMapInstanced*>("heatmapLayer");
    if (heatmapLayer) {
        connect(m_streamController, &StreamController::orderBookUpdated,
                heatmapLayer, &HeatMapInstanced::onOrderBookUpdated,
                Qt::QueuedConnection); // CRITICAL: Async connection for thread safety!
        
        LOG_I(CAT, "🔥 HEATMAP CONNECTED TO REAL-TIME ORDER BOOK DATA!");
    } else {
        LOG_W(CAT, "⚠️ HeatMapInstanced (heatmapLayer) not found in QML root - CHECK OBJECTNAME!");
    }
} 