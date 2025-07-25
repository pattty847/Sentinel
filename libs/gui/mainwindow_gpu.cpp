#include "mainwindow_gpu.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "chartmodecontroller.h"
#include "GridIntegrationAdapter.h"
#include "UnifiedGridRenderer.h"
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimer>

// 🎯 PHASE 5: Pure grid-only mode - legacy includes removed

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {
    qDebug() << "🎯 PHASE 5: Creating GPU Trading Terminal in pure GRID-ONLY mode!";
    
    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    
    // Automatically start the subscription process once the event loop begins.
    // This ensures the window is fully constructed and shown before we start streaming.
    QTimer::singleShot(0, this, &MainWindowGPU::onSubscribe);
    
    qDebug() << "✅ GPU MainWindow ready for 144Hz trading!";
}

MainWindowGPU::~MainWindowGPU() {
    qDebug() << "🛑 MainWindowGPU destructor - cleaning up...";
    
    // Stop data streams first
    if (m_streamController) {
        m_streamController->stop();
    }
    
    // Disconnect all signals to prevent callbacks during destruction
    if (m_gpuChart && m_gpuChart->rootObject()) {
        disconnect(m_gpuChart->rootObject(), nullptr, this, nullptr);
    }
    
    // Clear QML context to prevent dangling references
    if (m_gpuChart) {
        m_gpuChart->setSource(QUrl());
    }
    
    qDebug() << "✅ MainWindowGPU cleanup complete";
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
    // Phase 2: New single data pipeline architecture
    m_streamController = new StreamController(this);
    m_statsController = new StatisticsController(this);
    
    // NEW: GridIntegrationAdapter as primary data hub (replaces GPUDataAdapter)
    m_gridAdapter = new GridIntegrationAdapter(this);
    m_gridAdapter->setGridModeEnabled(true);  // 🎯 PHASE 5: Always grid-only mode

    qDebug() << "🎯 Phase 2: GridIntegrationAdapter created as primary data hub, mode:" << m_gridAdapter->gridModeEnabled();
    
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
    
    // 🎯 PHASE 3 FIX: Expose C++ GridIntegrationAdapter to QML
    context->setContextProperty("gridIntegrationAdapter", m_gridAdapter);
    
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
    qDebug() << "🔍 ATTEMPTING TO CONNECT TO GPU CHART... (attempt" << m_connectionRetryCount + 1 << "/" << MAX_CONNECTION_RETRIES << ")";
    
    // Check retry limit
    if (m_connectionRetryCount >= MAX_CONNECTION_RETRIES) {
        qCritical() << "❌ MAX CONNECTION RETRIES REACHED - GPU chart connection failed!";
        return;
    }
    
    // Get the GPU chart widget from QML
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        qWarning() << "❌ QML root object not found - retrying in 100ms...";
        m_connectionRetryCount++;
        QTimer::singleShot(100, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    qDebug() << "✅ QML root found:" << qmlRoot;
    
    // 🎯 PHASE 5: Pure grid-only component lookup
    UnifiedGridRenderer* unifiedGridRenderer = qmlRoot->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    GridIntegrationAdapter* qmlGridAdapter = qmlRoot->findChild<GridIntegrationAdapter*>("gridIntegration");
    
    // 🎯 PHASE 5: Pure grid-only validation - only require UnifiedGridRenderer
    if (!unifiedGridRenderer) {
        qWarning() << "❌ UnifiedGridRenderer not found - retrying in 200ms...";
        m_connectionRetryCount++;
        QTimer::singleShot(200, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    qDebug() << "✅ Pure grid-only mode validation passed";
    
    // 🎯 PHASE 2: NEW SINGLE DATA PIPELINE - StreamController → GridIntegrationAdapter
    bool allConnectionsSuccessful = true;
    
    // PRIMARY: Connect StreamController to GridIntegrationAdapter (single data hub)
    bool primaryTradeConnection = connect(m_streamController, &StreamController::tradeReceived,
                                         m_gridAdapter, &GridIntegrationAdapter::onTradeReceived,
                                         Qt::QueuedConnection);
    bool primaryOrderBookConnection = connect(m_streamController, &StreamController::orderBookUpdated,
                                             m_gridAdapter, &GridIntegrationAdapter::onOrderBookUpdated,
                                             Qt::QueuedConnection);
    allConnectionsSuccessful &= primaryTradeConnection && primaryOrderBookConnection;
    
    // 🎯 PHASE 5: Pure grid-only connection - Connect GridIntegrationAdapter to UnifiedGridRenderer
    if (unifiedGridRenderer) {
        m_gridAdapter->setGridRenderer(unifiedGridRenderer);
        qDebug() << "✅ GridIntegrationAdapter connected to UnifiedGridRenderer";
    }
    
    // 🎯 PHASE 5: Legacy connections permanently removed - pure grid-only mode
    
    // 🎯 PHASE 5: Pure grid-only data pipeline summary
    qDebug() << "🎯 Phase 5 Pure Grid-Only Data Pipeline Summary:"
             << "Primary connections:" << (primaryTradeConnection && primaryOrderBookConnection ? "SUCCESS" : "FAILED")
             << "Grid renderer:" << (unifiedGridRenderer ? "CONNECTED" : "MISSING");
    
    // 🔥 FINAL STATUS REPORT
    if (allConnectionsSuccessful) {
        qDebug() << "✅ PURE GRID-ONLY TERMINAL FULLY CONNECTED TO REAL-TIME DATA PIPELINE!";
        qDebug() << "🎯 Grid components:" 
                 << "UnifiedGridRenderer:" << (unifiedGridRenderer ? "YES" : "NO")
                 << "GridIntegration:" << (qmlGridAdapter ? "YES" : "NO");
        m_connectionRetryCount = 0;  // Reset retry counter on success
    } else {
        qWarning() << "⚠️ Some GPU connections failed - partial functionality";
    }
} 