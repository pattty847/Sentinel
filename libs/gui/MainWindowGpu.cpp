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
#include <QQuickView>
#include <QLabel>
#include <QLineEdit>
#include "ChartModeController.h"
#include "MainWindowGpu.h"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"
#include "SentinelLogging.hpp"
#include <QQmlContext>
#include <QMetaObject>
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
    
    sLog_App("Creating persistent MarketDataCore...");
    m_marketDataCore = std::make_unique<MarketDataCore>(*m_authenticator, *m_dataCache, m_sentinelMonitor);
    m_marketDataCore->start();
    sLog_App("MarketDataCore created and started");
    
    sLog_App("Setting up UI...");
    setupUI();
    sLog_App("UI setup complete");
    
    sLog_App("Setting up connections...");
    setupConnections();
    sLog_App("Connections setup complete");
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    sLog_App("Window properties set");
    
    sLog_App("Initializing QML components...");
    initializeQMLComponents();
    sLog_App("QML components initialized");
    
    sLog_App("Connecting MarketData signals...");
    connectMarketDataSignals();
    sLog_App("MarketData signals connected");
    
    sLog_App("GPU MainWindow ready for 144Hz trading!");
}

MainWindowGPU::~MainWindowGPU() {
    sLog_App("MainWindowGPU destructor - cleaning up...");
    
    // This prevents queued signals from reaching DataProcessor during shutdown
    if (m_marketDataCore) {
        disconnect(m_marketDataCore.get(), nullptr, nullptr, nullptr);
    }
    
    // This ensures cleanup happens synchronously, not async after QML destruction
    if (m_qquickView && m_qquickView->rootObject()) {
        UnifiedGridRenderer* unifiedGridRenderer = m_qquickView->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
        if (unifiedGridRenderer) {
            auto dataProcessor = unifiedGridRenderer->getDataProcessor();
            if (dataProcessor) {
                sLog_App("Stopping DataProcessor from MainWindowGPU...");
                dataProcessor->stopProcessing();
            }
        }
    }
    
    if (m_marketDataCore) {
        m_marketDataCore->stop();
        m_marketDataCore.reset();
        sLog_App("MarketDataCore destroyed");
    }
    
    // This will trigger UnifiedGridRenderer destructor
    if (m_qquickView) {
        m_qquickView->setSource(QUrl());
    }

    if (m_sentinelMonitor) {
        m_sentinelMonitor->stopMonitoring();
    }

    sLog_App("MainWindowGPU cleanup complete");
}

void MainWindowGPU::setupUI() {
    // Create GPU Chart (QML) with threaded scene graph via QQuickView
    m_qquickView = new QQuickView;
    m_qquickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_qquickView->setColor(Qt::black);
    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, this);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    
    // Allow environment override for QML path (SENTINEL_QML_PATH)
    QString qmlPath;
    const QString qmlEnv = qEnvironmentVariable("SENTINEL_QML_PATH");
    if (!qmlEnv.isEmpty()) {
        QFileInfo envInfo(qmlEnv);
        qmlPath = envInfo.isDir()
            ? QDir(envInfo.absoluteFilePath()).filePath("DepthChartView.qml")
            : envInfo.absoluteFilePath();
        sLog_App("Using SENTINEL_QML_PATH override:" << qmlPath);
    } else {
        // Try file system path first to bypass resource issues
        // TODO: move this to a config file - OS dependent paths
        qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
        sLog_App("Trying QML path:" << qmlPath);
    }
    
    if (QFile::exists(qmlPath)) {
        m_qquickView->setSource(QUrl::fromLocalFile(qmlPath));
        sLog_App("QML loaded from file system!");
    } else {
        sLog_App("QML file not found, trying resource path...");
        m_qquickView->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    // Check if QML loaded successfully
    if (m_qquickView->status() == QQuickView::Error) {
        sLog_Error("QML FAILED TO LOAD!");
        sLog_Error("QML Errors:" << m_qquickView->errors());
    }
    
    // Set default symbol in QML context
    QQmlContext* context = m_qquickView->rootContext();
    context->setContextProperty("symbol", "BTC-USD");
    m_modeController = new ChartModeController(this);
    context->setContextProperty("chartModeController", m_modeController);
    
    // Control panel
    m_statusLabel = new QLabel("Disconnected", this);
    m_symbolInput = new QLineEdit("BTC-USD", this);
    m_subscribeButton = new QPushButton("Subscribe", this);
    
    // Styling
    m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
    m_symbolInput->setStyleSheet("QLineEdit { padding: 8px; font-size: 14px; }");
    m_subscribeButton->setStyleSheet("QPushButton { padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    // Layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // GPU chart takes most space (QQuickView container)
    mainLayout->addWidget(m_qmlContainer, 1);
    
    // Control panel at bottom
    QGroupBox* controlGroup = new QGroupBox("Trading Controls");
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->addWidget(new QLabel("Symbol:"));
    controlLayout->addWidget(m_symbolInput);
    controlLayout->addWidget(m_subscribeButton);
    controlLayout->addStretch();
    controlLayout->addWidget(m_statusLabel);
    controlGroup->setLayout(controlLayout);
    
    mainLayout->addWidget(controlGroup);
    setLayout(mainLayout);
}

void MainWindowGPU::setupConnections() {
    // UI connections
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    
    sLog_App("GPU MainWindow basic connections established");
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        sLog_Warning("Empty symbol input");
        return;
    }
    
    sLog_App(QString("Subscribing to symbol: %1").arg(symbol));
    
    // Update QML context with the new symbol
    if (m_qquickView) m_qquickView->rootContext()->setContextProperty("symbol", symbol);
    
    //  GEMINI'S REFACTOR: Use persistent MarketDataCore with subscribeToSymbols!
    if (m_marketDataCore) {
        m_marketDataCore->subscribeToSymbols({symbol.toStdString()});
    } else {
        sLog_Error("MarketDataCore not initialized!");
    }
}

void MainWindowGPU::connectMarketDataSignals() {
    sLog_App("Setting up persistent MarketDataCore signal connections...");
    
    UnifiedGridRenderer* unifiedGridRenderer = m_qquickView->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    if (m_marketDataCore && unifiedGridRenderer) {
        // Provide dense data access to renderer
        unifiedGridRenderer->setDataCache(m_dataCache.get());
        
        // Provide unified monitoring to renderer
        unifiedGridRenderer->setSentinelMonitor(m_sentinelMonitor);
        
        // Get DataProcessor from UGR to route signals correctly
        auto dataProcessor = unifiedGridRenderer->getDataProcessor();
        if (dataProcessor) {
            // Route LiveOrderBook signal to DataProcessor (THE FIX!)
            connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookUpdated,
                    dataProcessor, &DataProcessor::onLiveOrderBookUpdated, Qt::QueuedConnection);
            sLog_App("LiveOrderBook signal routed to DataProcessor!");
        }
        
        // Route trades to renderer
        connect(
            m_marketDataCore.get(),
            &MarketDataCore::tradeReceived,
            this,
            [unifiedGridRenderer](const Trade& trade) {
                const Trade tradeCopy = trade;
                QMetaObject::invokeMethod(
                    unifiedGridRenderer,
                    [unifiedGridRenderer, tradeCopy]() {
                        unifiedGridRenderer->onTradeReceived(tradeCopy);
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
        
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
        
        sLog_App("Persistent MarketDataCore → DataProcessor connections established!");
    } else {
        sLog_Error("MarketDataCore or UnifiedGridRenderer not found during signal setup!");
        if (!m_marketDataCore) sLog_Error("  MarketDataCore is null!");
        if (!unifiedGridRenderer) sLog_Error("  UnifiedGridRenderer not found!");
    }
}

// Proper QML component initialization without retry logic
void MainWindowGPU::initializeQMLComponents() {
    sLog_App("Initializing QML components with proper lifecycle management");
    
    // Check if QML loaded successfully during setupUI()
    if (m_qquickView->status() == QQuickView::Error) {
        sLog_Error("QML failed to load during setupUI - cannot initialize components");
        return;
    }
    
    // Verify QML root object is available (should be ready after setupUI())
    QQuickItem* qmlRoot = m_qquickView->rootObject();
    if (!qmlRoot) {
        sLog_Warning("QML root object not available - QML may not be fully loaded");
        return;
    }
    
    // Verify UnifiedGridRenderer is available
    UnifiedGridRenderer* unifiedGridRenderer = qmlRoot->findChild<UnifiedGridRenderer*>("unifiedGridRenderer");
    if (!unifiedGridRenderer) {
        sLog_Warning("UnifiedGridRenderer not found in QML");
        return;
    }
    
    sLog_App("QML components verified and ready");
    sLog_App("QML Root: AVAILABLE");
    sLog_App("UnifiedGridRenderer: AVAILABLE");
    
    // Components are verified - data pipeline will be created on subscription
}
