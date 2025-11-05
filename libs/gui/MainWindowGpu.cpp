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
#include <QMessageBox>  // Added for error dialogs
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
#include <QSGRendererInterface>
#include <QSettings>  // For config extraction
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScopeGuard>

// Helper macro for scoped logging
#define LOG_SCOPE(msg) sLog_App(msg " started"); auto _scopeGuard = qScopeGuard([=]{ sLog_App(msg " complete"); });

MainWindowGPU::MainWindowGPU(QWidget* parent) : QWidget(parent) {
    LOG_SCOPE("MainWindowGPU construction");
    
    initializeDataComponents();
    setupUI();
    setupConnections();
    setWindowProperties();
    
    if (!validateComponents()) {
        sLog_Error("Component validation failed - app may not function correctly");
        QMessageBox::critical(this, "Initialization Error", "Failed to initialize core components. Check logs.");
    }
    
    sLog_App("GPU MainWindow ready for 144Hz trading!");
}

MainWindowGPU::~MainWindowGPU() {
    LOG_SCOPE("MainWindowGPU destruction");
    
    // Disconnect signals first
    if (m_marketDataCore) {
        disconnect(m_marketDataCore.get(), nullptr, nullptr, nullptr);
        m_marketDataCore->stop();
        m_marketDataCore.reset();
    }
    
    // Stop processing
    auto unifiedGridRenderer = getUnifiedGridRenderer();
    if (unifiedGridRenderer) {
        auto dataProcessor = unifiedGridRenderer->getDataProcessor();
        if (dataProcessor) dataProcessor->stopProcessing();
    }
    
    if (m_qquickView) m_qquickView->setSource(QUrl());  // Clear QML
    
    if (m_sentinelMonitor) m_sentinelMonitor->stopMonitoring();
}

void MainWindowGPU::initializeDataComponents() {
    LOG_SCOPE("Initializing data components");
    
    // Load config (example: extract to separate class if complex)
    QSettings config("config.ini", QSettings::IniFormat);
    QString defaultKeyFile = config.value("auth/keyFile", "key.json").toString();
    
    m_authenticator = std::make_unique<Authenticator>(defaultKeyFile.toStdString());
    m_dataCache = std::make_unique<DataCache>();
    m_sentinelMonitor = std::make_shared<SentinelMonitor>();
    m_sentinelMonitor->startMonitoring();
    m_sentinelMonitor->enableCLIOutput(false);
    
    m_marketDataCore = std::make_unique<MarketDataCore>(*m_authenticator, *m_dataCache, m_sentinelMonitor);
    m_marketDataCore->start();
}

void MainWindowGPU::setupUI() {
    LOG_SCOPE("Setting up UI");
    
    m_qquickView = new QQuickView;
    m_qquickView->setPersistentSceneGraph(true);
    m_qquickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_qquickView->setColor(Qt::black);
    
    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, this);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    
    loadQmlSource();
    verifyGpuAcceleration();
    
    QQmlContext* context = m_qquickView->rootContext();
    m_modeController = new ChartModeController(this);
    context->setContextProperty("chartModeController", m_modeController);
    updateSymbolInContext("BTC-USD");  // Centralized
    
    // Control panel
    m_statusLabel = new QLabel("Disconnected", this);
    m_symbolInput = new QLineEdit("BTC-USD", this);
    m_subscribeButton = new QPushButton("Subscribe", this);
    
    applyStyles();  // Extracted
    
    // Layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_qmlContainer, 1);
    
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

void MainWindowGPU::loadQmlSource() {
    // Configurable path (extract from config)
    QSettings config("config.ini", QSettings::IniFormat);
    QString qmlPath = config.value("qml/path", QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath())).toString();
    
    const QString qmlEnv = qEnvironmentVariable("SENTINEL_QML_PATH");
    if (!qmlEnv.isEmpty()) qmlPath = qmlEnv;  // Override
    
    if (QFile::exists(qmlPath)) {
        m_qquickView->setSource(QUrl::fromLocalFile(qmlPath));
    } else {
        m_qquickView->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    if (m_qquickView->status() == QQuickView::Error) {
        sLog_Error("QML FAILED TO LOAD! Errors: " << m_qquickView->errors());
        QMessageBox::critical(this, "QML Error", "Failed to load QML. Check logs for details.");
    }
}

void MainWindowGPU::verifyGpuAcceleration() {
    if (m_qquickView->status() != QQuickView::Ready) return;
    
    auto* rhi = m_qquickView->rendererInterface();
    if (rhi && rhi->graphicsApi() != QSGRendererInterface::Null) {
        sLog_App("🎮 GPU ACCELERATION ACTIVE: " << graphicsApiName(rhi->graphicsApi()));  // Helper function for name
    } else {
        sLog_Error("❌ NO GPU ACCELERATION!");
    }
}

QString MainWindowGPU::graphicsApiName(QSGRendererInterface::GraphicsApi api) {  // Helper
    switch (api) {
        case QSGRendererInterface::OpenGL: return "OpenGL";
        case QSGRendererInterface::Direct3D11: return "Direct3D 11";
        case QSGRendererInterface::Vulkan: return "Vulkan";
        case QSGRendererInterface::Metal: return "Metal";
        default: return "Unknown";
    }
}

void MainWindowGPU::applyStyles() {  // Extracted for modularity
    m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
    m_symbolInput->setStyleSheet("QLineEdit { padding: 8px; font-size: 14px; }");
    m_subscribeButton->setStyleSheet("QPushButton { padding: 8px 16px; font-size: 14px; font-weight: bold; }");
}

void MainWindowGPU::setWindowProperties() {
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);  // Could be from config
}

void MainWindowGPU::setupConnections() {
    LOG_SCOPE("Setting up connections");
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    connectMarketDataSignals();
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty() || !symbol.contains('-')) {  // Basic validation
        sLog_Warning("Invalid symbol: " << symbol);
        QMessageBox::warning(this, "Invalid Input", "Enter a valid symbol like BTC-USD.");
        return;
    }
    
    sLog_App("Subscribing to: " << symbol);
    updateSymbolInContext(symbol);
    if (m_marketDataCore) {
        m_marketDataCore->subscribeToSymbols({symbol.toStdString()});
    }
}

void MainWindowGPU::updateSymbolInContext(const QString& symbol) {
    if (m_qquickView) m_qquickView->rootContext()->setContextProperty("symbol", symbol);
}

void MainWindowGPU::connectMarketDataSignals() {
    LOG_SCOPE("Connecting MarketData signals");
    
    auto unifiedGridRenderer = getUnifiedGridRenderer();
    if (!m_marketDataCore || !unifiedGridRenderer) {
        sLog_Error("Cannot connect signals: Missing components");
        return;
    }
    
    unifiedGridRenderer->setDataCache(m_dataCache.get());
    unifiedGridRenderer->setSentinelMonitor(m_sentinelMonitor);
    
    auto dataProcessor = unifiedGridRenderer->getDataProcessor();
    if (dataProcessor) {
        connect(m_marketDataCore.get(), &MarketDataCore::liveOrderBookUpdated,
                dataProcessor, &DataProcessor::onLiveOrderBookUpdated, Qt::QueuedConnection);
    }
    
    // Trade connection (simplified, assume UnifiedGridRenderer has a slot)
    connect(m_marketDataCore.get(), &MarketDataCore::tradeReceived,
            unifiedGridRenderer, &UnifiedGridRenderer::onTradeReceived, Qt::QueuedConnection);
    
    connect(m_marketDataCore.get(), &MarketDataCore::connectionStatusChanged,
            this, &MainWindowGPU::onConnectionStatusChanged);  // Extracted slot
}

void MainWindowGPU::onConnectionStatusChanged(bool connected) {  // Extracted for clarity
    m_statusLabel->setText(connected ? "🟢 Connected" : "🔴 Disconnected");
    m_statusLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 14px; }").arg(connected ? "green" : "red"));
    m_subscribeButton->setText(connected ? "Subscribe" : "Connect");
    m_subscribeButton->setEnabled(true);
}

bool MainWindowGPU::validateComponents() {
    if (m_qquickView->status() != QQuickView::Ready) return false;
    if (!getUnifiedGridRenderer()) return false;
    if (!m_marketDataCore) return false;
    return true;
}

UnifiedGridRenderer* MainWindowGPU::getUnifiedGridRenderer() const {
    return m_qquickView->rootObject() ? m_qquickView->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer") : nullptr;
}