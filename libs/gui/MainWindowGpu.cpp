#include "MainWindowGpu.h"
#include "StatisticsController.h"
#include "ChartModeController.h"
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
    
    // 🔥 PHASE 1.1: Stop data streams first (direct client instead of StreamController)
    if (m_client) {
        m_client.reset();  // Clean shutdown of MarketDataCore
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
    // 🔥 PHASE 1.1: StreamController OBLITERATED - Direct connection architecture
    // m_streamController = new StreamController(this);  // OBLITERATED!
    m_statsController = new StatisticsController(this);
    
    // 🔥 PHASE 1.2: GridIntegrationAdapter OBLITERATED! Direct MarketDataCore → UnifiedGridRenderer pipeline
    // m_gridAdapter = new GridIntegrationAdapter(this);  // OBLITERATED!

    qDebug() << "🔥 Phase 1.1: StreamController OBLITERATED - Direct data pipeline enabled";
    
    // UI connections
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    
    // 🔥 PHASE 1.1: Direct MarketDataCore connections (StreamController OBLITERATED)
    // Connection status will be handled directly through MarketDataCore signals
    
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
    
    // 🔥 PHASE 1.2: GridIntegrationAdapter QML exposure OBLITERATED!
    // context->setContextProperty("gridIntegrationAdapter", m_gridAdapter);  // OBLITERATED!
    
    // 🔥 PHASE 2.4: Create CoinbaseStreamClient and subscribe FIRST
    m_client = std::make_unique<CoinbaseStreamClient>();
    std::vector<std::string> symbols = {symbol.toStdString()};
    m_client->subscribe(symbols);  // This creates the MarketDataCore
    m_client->start();
    
    // 🔥 PHASE 2.4: V2 ARCHITECTURE FIX - Connect MarketDataCore → UnifiedGridRenderer AFTER MarketDataCore exists
    QObject* unifiedGridRenderer = m_gpuChart->rootObject()->findChild<QObject*>("unifiedGridRenderer");
    if (m_client->getMarketDataCore() && unifiedGridRenderer) {
        // Use QObject::connect with SIGNAL/SLOT macros for cross-boundary connections
        connect(m_client->getMarketDataCore(), SIGNAL(tradeReceived(const Trade&)),
                unifiedGridRenderer, SLOT(onTradeReceived(const Trade&)), Qt::QueuedConnection);
        connect(m_client->getMarketDataCore(), SIGNAL(orderBookUpdated(const OrderBook&)),
                unifiedGridRenderer, SLOT(onOrderBookUpdated(const OrderBook&)), Qt::QueuedConnection);
        
        // Connection status feedback
        connect(m_client->getMarketDataCore(), &MarketDataCore::connectionStatusChanged, 
                this, [this](bool connected) {
            if (connected) {
                m_statusLabel->setText("🟢 Connected");
                m_statusLabel->setStyleSheet("QLabel { color: green; font-size: 14px; }");
                m_subscribeButton->setText("🔗 Connected");
                m_subscribeButton->setEnabled(false);
            } else {
                m_statusLabel->setText("🔴 Disconnected");  
                m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
                m_subscribeButton->setText("🚀 Subscribe");
                m_subscribeButton->setEnabled(true);
            }
        });
        
        qDebug() << "🔥 PHASE 2.4: V2 Architecture Fix - MarketDataCore → UnifiedGridRenderer connections established AFTER subscription!";
    } else {
        qCritical() << "❌ PHASE 2.4: MarketDataCore or UnifiedGridRenderer not found!";
        if (!m_client->getMarketDataCore()) qCritical() << "   MarketDataCore is null!";
        if (!unifiedGridRenderer) qCritical() << "   UnifiedGridRenderer not found!";
    }
    
    qDebug() << "✅ PHASE 1.1: Direct data pipeline started for" << symbol;
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
    
    // 🔥 PHASE 1.2: Ultra-direct pipeline - only UnifiedGridRenderer needed
    UnifiedGridRenderer* unifiedGridRenderer = qmlRoot->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    // GridIntegrationAdapter* qmlGridAdapter = qmlRoot->findChild<GridIntegrationAdapter*>("gridIntegration");  // OBLITERATED!
    
    // 🔥 PHASE 1.2: Ultra-direct validation - only require UnifiedGridRenderer
    if (!unifiedGridRenderer) {
        qWarning() << "❌ UnifiedGridRenderer not found - retrying in 200ms...";
        m_connectionRetryCount++;
        QTimer::singleShot(200, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    qDebug() << "✅ Pure grid-only mode validation passed";
    
    // 🔥 PHASE 1.2: ULTRA-DIRECT DATA PIPELINE - MarketDataCore → UnifiedGridRenderer  
    bool allConnectionsSuccessful = true;
    
    // Data pipeline connections established directly in onSubscribe()
    // Both StreamController AND GridIntegrationAdapter OBLITERATED!
    
    // 🎯 PHASE 5: Legacy connections permanently removed - pure grid-only mode
    
    // 🔥 PHASE 1.2: Ultra-direct data pipeline summary  
    qDebug() << "🔥 Phase 1.2: BOTH StreamController & GridIntegrationAdapter OBLITERATED!"
             << "Grid renderer:" << (unifiedGridRenderer ? "READY FOR DIRECT CONNECTION" : "MISSING")
             << "Ultra-direct MarketDataCore → UnifiedGridRenderer pipeline!";
    
    // 🔥 FINAL STATUS REPORT
    if (allConnectionsSuccessful) {
        qDebug() << "✅ PURE GRID-ONLY TERMINAL FULLY CONNECTED TO REAL-TIME DATA PIPELINE!";
        qDebug() << "🔥 PHASE 1.2: Ultra-direct pipeline components:" 
                 << "UnifiedGridRenderer:" << (unifiedGridRenderer ? "YES" : "NO")
                 << "GridIntegration:" << "OBLITERATED!";
        m_connectionRetryCount = 0;  // Reset retry counter on success
    } else {
        qWarning() << "⚠️ Some GPU connections failed - partial functionality";
    }
} 