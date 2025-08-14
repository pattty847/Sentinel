/*
Sentinel — MainWindowGpu
Role: Implements UI setup and the core data-to-rendering pipeline connection logic.
Inputs/Outputs: Connects MarketDataCore signals to UnifiedGridRenderer slots.
Threading: Runs on the main GUI thread, using QueuedConnections for thread safety.
Performance: Connection logic is part of the user-initiated subscription setup.
Integration: Obtains MarketDataCore from CoinbaseStreamClient and wires it to the QML renderer.
Observability: Detailed logging of UI/QML initialization and data pipeline status via sLog_App/sLog_Data.
Related: MainWindowGpu.h, UnifiedGridRenderer.h, CoinbaseStreamClient.hpp.
Assumptions: MarketDataCore becomes available from the client after subscribe() is called.
*/

#include "MainWindowGpu.h"
#include "StatisticsController.h"
#include "ChartModeController.h"
#include "UnifiedGridRenderer.h"
#include "SentinelLogging.hpp"
#include <QQmlContext>
#include <QDir>
#include <QFile>
#include <QTimer>

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {
    qRegisterMetaType<std::shared_ptr<const OrderBook>>("std::shared_ptr<const OrderBook>");
    
    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    
    // Automatically start the subscription process once the event loop begins.
    // This ensures the window is fully constructed and shown before we start streaming.
    QTimer::singleShot(0, this, &MainWindowGPU::onSubscribe);
    
    sLog_App("✅ GPU MainWindow ready for 144Hz trading!");
}

MainWindowGPU::~MainWindowGPU() {
    sLog_App("🛑 MainWindowGPU destructor - cleaning up...");
    
    // Stop data streams first
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
    
    sLog_App("✅ MainWindowGPU cleanup complete");
}

void MainWindowGPU::setupUI() {
    // Create GPU Chart (QML) - Load from file system first
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    
    // Try file system path first to bypass resource issues
    // TODO: move this to a config file - OS dependent paths
    QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
    sLog_App("🔍 Trying QML path:" << qmlPath);
    
    if (QFile::exists(qmlPath)) {
        m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        sLog_App("✅ QML loaded from file system!");
    } else {
        sLog_App("❌ QML file not found, trying resource path...");
        m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    // Check if QML loaded successfully
    if (m_gpuChart->status() == QQuickWidget::Error) {
        sLog_Error("🚨 QML FAILED TO LOAD!");
        sLog_Error("QML Errors:" << m_gpuChart->errors());
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
    controlLayout->addStretch();
    controlLayout->addWidget(m_cvdLabel);
    controlLayout->addWidget(m_statusLabel);
    controlGroup->setLayout(controlLayout);
    
    mainLayout->addWidget(controlGroup);
    setLayout(mainLayout);
}

void MainWindowGPU::setupConnections() {
    m_statsController = new StatisticsController(this);

    // UI connections
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    
    // Stats pipeline
    connect(m_statsController, &StatisticsController::cvdUpdated, 
            this, &MainWindowGPU::onCVDUpdated);
    
    // Establish GPU connections
    connectToGPUChart();
    
    sLog_App("✅ GPU MainWindow connections established");
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        sLog_Warning("❌ Empty symbol input");
        return;
    }
    
    sLog_App("🚀 Subscribing to GPU chart:" << symbol);
    
    // Update QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", symbol);
    
    // Create CoinbaseStreamClient and subscribe FIRST
    m_client = std::make_unique<CoinbaseStreamClient>();
    std::vector<std::string> symbols = {symbol.toStdString()};
    m_client->subscribe(symbols);  // This creates the MarketDataCore
    m_client->start();
    
    // V2 ARCHITECTURE FIX - Connect MarketDataCore → UnifiedGridRenderer AFTER MarketDataCore exists
    UnifiedGridRenderer* unifiedGridRenderer = m_gpuChart->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    if (m_client->getMarketDataCore() && unifiedGridRenderer) {
        // Use QObject::connect with SIGNAL/SLOT macros for cross-boundary connections
        connect(m_client->getMarketDataCore(), SIGNAL(tradeReceived(const Trade&)),
                unifiedGridRenderer, SLOT(onTradeReceived(const Trade&)), Qt::QueuedConnection);
        connect(m_client->getMarketDataCore(), &MarketDataCore::orderBookUpdated,
                unifiedGridRenderer, &UnifiedGridRenderer::onOrderBookUpdated, Qt::QueuedConnection);
        
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
        
        sLog_App("🔥 V2 Architecture Fix - MarketDataCore → UnifiedGridRenderer connections established AFTER subscription!");
    } else {
        sLog_Error("❌ MarketDataCore or UnifiedGridRenderer not found!");
        if (!m_client->getMarketDataCore()) sLog_Error("   MarketDataCore is null!");
        if (!unifiedGridRenderer) sLog_Error("   UnifiedGridRenderer not found!");
    }
    
    sLog_App("✅ Direct data pipeline started for" << symbol);
}

void MainWindowGPU::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

// Helper method to establish GPU connections
void MainWindowGPU::connectToGPUChart() {
    sLog_Debug("🔍 ATTEMPTING TO CONNECT TO GPU CHART... (attempt" << m_connectionRetryCount + 1 << "/" << MAX_CONNECTION_RETRIES << ")");
    
    // Check retry limit
    if (m_connectionRetryCount >= MAX_CONNECTION_RETRIES) {
        sLog_Error("❌ MAX CONNECTION RETRIES REACHED - GPU chart connection failed!");
        return;
    }
    
    // Get the GPU chart widget from QML
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        sLog_Warning("❌ QML root object not found - retrying in 100ms...");
        m_connectionRetryCount++;
        QTimer::singleShot(100, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    sLog_Debug("✅ QML root found:" << qmlRoot);
    
    // Ultra-direct pipeline - only UnifiedGridRenderer needed
    UnifiedGridRenderer* unifiedGridRenderer = qmlRoot->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    
    // Ultra-direct validation - only require UnifiedGridRenderer
    if (!unifiedGridRenderer) {
        sLog_Warning("❌ UnifiedGridRenderer not found - retrying in 200ms...");
        m_connectionRetryCount++;
        QTimer::singleShot(200, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    sLog_Debug("✅ Pure grid-only mode validation passed");
    
    // ULTRA-DIRECT DATA PIPELINE - MarketDataCore → UnifiedGridRenderer  
    bool allConnectionsSuccessful = true;

    // FINAL STATUS REPORT
    if (allConnectionsSuccessful) {
        sLog_App("🔥 Ultra-direct pipeline components:" << "UnifiedGridRenderer:" << (unifiedGridRenderer ? "YES" : "NO"));
        m_connectionRetryCount = 0;  // Reset retry counter on success
    } else {
        sLog_Warning("⚠️ Some GPU connections failed - partial functionality");
    }
} 