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
#include <QQuickWidget>
#include <QLabel>
#include <QLineEdit>
#include "ChartModeController.h"
#include "MainWindowGpu.h"
#include "StatisticsController.h"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"  // 🚀 PHASE 3: Include for signal routing
#include "SentinelLogging.hpp"
#include <QQmlContext>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {    
    // Initialize data components (was previously in facade)
    m_authenticator = std::make_unique<Authenticator>();  // uses default "key.json"
    m_dataCache = std::make_unique<DataCache>();
    
    m_sentinelMonitor = std::make_shared<SentinelMonitor>();
    m_sentinelMonitor->startMonitoring();
    m_sentinelMonitor->enableCLIOutput(true);  // Enable performance logging
    
    sLog_App("🔧 Creating persistent MarketDataCore...");
    m_marketDataCore = std::make_unique<MarketDataCore>(*m_authenticator, *m_dataCache, m_sentinelMonitor);
    m_marketDataCore->start();
    sLog_App("✅ MarketDataCore created and started");
    
    sLog_App("🔧 Setting up UI...");
    setupUI();
    sLog_App("✅ UI setup complete");
    
    sLog_App("🔧 Setting up connections...");
    setupConnections();
    sLog_App("✅ Connections setup complete");
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    sLog_App("✅ Window properties set");
    
    sLog_App("🔧 Initializing QML components...");
    initializeQMLComponents();
    sLog_App("✅ QML components initialized");
    
    sLog_App("🔧 Connecting MarketData signals...");
    // 🚀 PHASE 3: Connect signals after QML components are ready
    connectMarketDataSignals();
    sLog_App("✅ MarketData signals connected");
    
    sLog_App("✅ GPU MainWindow ready for 144Hz trading!");
}

MainWindowGPU::~MainWindowGPU() {
    sLog_App("🛑 MainWindowGPU destructor - cleaning up...");
    
    // Stop data streams first
    if (m_marketDataCore) {
        m_marketDataCore->stop();
        m_marketDataCore.reset();  // Clean shutdown of MarketDataCore
    }
    
    // Disconnect all signals to prevent callbacks during destruction
    if (m_gpuChart && m_gpuChart->rootObject()) {
        disconnect(m_gpuChart->rootObject(), nullptr, this, nullptr);
    }
    
    // Clear QML context to prevent dangling references
    if (m_gpuChart) {
        m_gpuChart->setSource(QUrl());
    }

    if (m_sentinelMonitor) {
        m_sentinelMonitor->stopMonitoring();
    }

    sLog_App("✅ MainWindowGPU cleanup complete");
}

void MainWindowGPU::setupUI() {
    // Create GPU Chart (QML) - Load from file system first
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    
    // Allow environment override for QML path (SENTINEL_QML_PATH)
    QString qmlPath;
    const QString qmlEnv = qEnvironmentVariable("SENTINEL_QML_PATH");
    if (!qmlEnv.isEmpty()) {
        QFileInfo envInfo(qmlEnv);
        qmlPath = envInfo.isDir()
            ? QDir(envInfo.absoluteFilePath()).filePath("DepthChartView.qml")
            : envInfo.absoluteFilePath();
        sLog_App("🔍 Using SENTINEL_QML_PATH override:" << qmlPath);
    } else {
        // Try file system path first to bypass resource issues
        // TODO: move this to a config file - OS dependent paths
        qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
        sLog_App("🔍 Trying QML path:" << qmlPath);
    }
    
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
    
    // PHASE 1.2: Remove scattered QML connection setup - moved to constructor-based initialization
    sLog_App("✅ GPU MainWindow basic connections established");
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        sLog_Warning("❌ Empty symbol input");
        return;
    }
    
    sLog_App(QString("🚀 Subscribing to symbol: %1").arg(symbol));
    
    // Update QML context with the new symbol
    m_gpuChart->rootContext()->setContextProperty("symbol", symbol);
    
    // 🚀 GEMINI'S REFACTOR: Use persistent MarketDataCore with subscribeToSymbols!
    if (m_marketDataCore) {
        m_marketDataCore->subscribeToSymbols({symbol.toStdString()});
    } else {
        sLog_Error("❌ MarketDataCore not initialized!");
    }
}

void MainWindowGPU::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

// 🚀 GEMINI'S REFACTOR: Signal connections moved from onSubscribe to constructor
void MainWindowGPU::connectMarketDataSignals() {
    sLog_App("🔥 Setting up persistent MarketDataCore signal connections...");
    
    UnifiedGridRenderer* unifiedGridRenderer = m_gpuChart->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    if (m_marketDataCore && unifiedGridRenderer) {
        // PHASE 2.1: Provide dense data access to renderer
        unifiedGridRenderer->setDataCache(m_dataCache.get());
        
        // PHASE 2.2: Provide unified monitoring to renderer
        unifiedGridRenderer->setSentinelMonitor(m_sentinelMonitor);
        
        // Get DataProcessor from UGR to route signals correctly
        auto dataProcessor = unifiedGridRenderer->getDataProcessor();
        if (dataProcessor) {
            // 🚀 PHASE 3: Route LiveOrderBook signal to DataProcessor (THE FIX!)
            connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookUpdated,
                    dataProcessor, &DataProcessor::onLiveOrderBookUpdated, Qt::QueuedConnection);
            sLog_App("🚀 PHASE 3: LiveOrderBook signal routed to DataProcessor!");
        }
        
        // Trade signal still goes to UGR
        connect(m_marketDataCore.get(), SIGNAL(tradeReceived(const Trade&)),
                unifiedGridRenderer, SLOT(onTradeReceived(const Trade&)), Qt::QueuedConnection);
        
        // Connection status feedback
        connect(m_marketDataCore.get(), &MarketDataCore::connectionStatusChanged, 
                this, [this](bool connected) {
            if (connected) {
                m_statusLabel->setText("🟢 Connected");
                m_statusLabel->setStyleSheet("QLabel { color: green; font-size: 14px; }");
                m_subscribeButton->setText("Subscribe");
                m_subscribeButton->setEnabled(true);
            } else {
                m_statusLabel->setText("🔴 Disconnected");  
                m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
                m_subscribeButton->setText("Connect");
                m_subscribeButton->setEnabled(true);
            }
        });
        
        sLog_App("🔥 Persistent MarketDataCore → DataProcessor connections established!");
    } else {
        sLog_Error("❌ MarketDataCore or UnifiedGridRenderer not found during signal setup!");
        if (!m_marketDataCore) sLog_Error("   MarketDataCore is null!");
        if (!unifiedGridRenderer) sLog_Error("   UnifiedGridRenderer not found!");
    }
}

// PHASE 1.2: Proper QML component initialization without retry logic
void MainWindowGPU::initializeQMLComponents() {
    sLog_App("🔥 PHASE 1.2: Initializing QML components with proper lifecycle management");
    
    // Check if QML loaded successfully during setupUI()
    if (m_gpuChart->status() == QQuickWidget::Error) {
        sLog_Error("❌ QML failed to load during setupUI - cannot initialize components");
        return;
    }
    
    // Verify QML root object is available (should be ready after setupUI())
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        sLog_Warning("❌ QML root object not available - QML may not be fully loaded");
        return;
    }
    
    // Verify UnifiedGridRenderer is available
    UnifiedGridRenderer* unifiedGridRenderer = qmlRoot->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    if (!unifiedGridRenderer) {
        sLog_Warning("❌ UnifiedGridRenderer not found in QML");
        return;
    }
    
    sLog_App("✅ PHASE 1.2: QML components verified and ready");
    sLog_App("   → QML Root: AVAILABLE");
    sLog_App("   → UnifiedGridRenderer: AVAILABLE");
    
    // Components are verified - data pipeline will be created on subscription
}
