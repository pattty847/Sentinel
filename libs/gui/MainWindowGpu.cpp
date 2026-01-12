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
#include <QMessageBox>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QShortcut>
#include <QInputDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QShowEvent>
#include <QStatusBar>
#include "ChartModeController.h"
#include "MainWindowGpu.h"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"
#include "SentinelLogging.hpp"
#include "widgets/HeatmapDock.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/MarketDataPanel.hpp"
#include "widgets/SecFilingDock.hpp"
#include "widgets/LayoutManager.hpp"
#include "widgets/ServiceLocator.hpp"
#include "mainwindow/DataBootstrapper.h"
#include "mainwindow/DockFactory.h"
#include "mainwindow/QmlSceneController.h"
#include "mainwindow/LayoutOrchestrator.h"
#include "mainwindow/MenuBuilder.h"
#include "mainwindow/ShortcutBinder.h"
#include "datasources/LocalGridDataSource.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QQmlContext>
#include <QMetaObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QSGRendererInterface>
#include <QSettings>
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScopeGuard>
#include <QScreen>
#include <QApplication>

// Helper macro for scoped logging
#define LOG_SCOPE(msg) sLog_App(msg " started"); auto _scopeGuard = qScopeGuard([=]{ sLog_App(msg " complete"); });

MainWindowGPU::MainWindowGPU(QWidget* parent) : QMainWindow(parent) {
    LOG_SCOPE("MainWindowGPU construction");
    
    // 1) Initialize data components via DataBootstrapper
    auto dataComponents = DataBootstrapper::initialize();
    m_marketDataCore = std::move(dataComponents.marketDataCore);
    m_authenticator = std::move(dataComponents.authenticator);
    m_dataCache = std::move(dataComponents.dataCache);
    
    // Initialize Data Source (Abstraction Layer)
    bool useRemote = qEnvironmentVariableIsSet("SENTINEL_REMOTE");
    
    if (useRemote) {
        sLog_App("Starting in REMOTE mode (connecting to 127.0.0.1:8080)");
        auto remote = std::make_unique<RemoteGridDataSource>("127.0.0.1", "8080");
        remote->connectToServer();
        m_dataSource = std::move(remote);
    } else {
        sLog_App("Starting in LOCAL mode");
        m_dataSource = std::make_unique<LocalGridDataSource>(m_marketDataCore.get(), m_dataCache.get());
    }

    // Register services with ServiceLocator
    ServiceLocator::registerMarketDataCore(m_marketDataCore.get());
    ServiceLocator::registerDataCache(m_dataCache.get());
    
    // 2) Create dock widgets via DockFactory
    setupUI();
    
    // 3) Initialize QML scene controller
    m_qmlController = std::make_unique<QmlSceneController>(m_qquickView);
    m_qmlController->loadQmlSource();
    m_qmlController->verifyGpuAcceleration();
    
    // Set up QML context properties
    m_modeController = new ChartModeController(this);
    m_qmlController->setChartModeController(m_modeController);
    m_qmlController->updateSymbolInContext("BTC-USD");  // Default symbol
    
    // 4) Set up layout orchestrator
    // NOTE: We defer arrangeDefaultLayout() until after window is shown and maximized,
    // because resizeDocks() doesn't work correctly when window is at default 640x480 size.
    m_layoutOrchestrator = std::make_unique<LayoutOrchestrator>(this);
    
    // 5) Set up menu bar and shortcuts
    m_menuBuilder = std::make_unique<MenuBuilder>(menuBar());
    m_shortcutBinder = std::make_unique<ShortcutBinder>(this);
    setupMenuBar();
    setupShortcuts();
    
    // 6) Set up connections
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
    
    if (m_qmlController) {
        // QmlSceneController manages QML cleanup
    }
}

// Data components initialization moved to DataBootstrapper

void MainWindowGPU::setupUI() {
    LOG_SCOPE("Setting up UI");
    
    // Prevent repaint storms during setup
    setUpdatesEnabled(false);
    
    // Create docks via DockFactory
    DockFactory dockFactory(this);
    auto docks = dockFactory.createDocks();
    m_heatmapDock = docks.heatmapDock;
    m_statusBar = docks.statusBar;
    m_marketDataDock = docks.marketDataDock;
    m_secDock = docks.secDock;
    m_copenetDock = docks.copenetDock;
    m_aiCommentaryDock = docks.aiCommentaryDock;
    
    // Get QML view references
    m_qquickView = m_heatmapDock->qquickView();
    m_qmlContainer = m_heatmapDock->qmlContainer();
    
    // Get symbol controls
    auto symbolControls = dockFactory.getSymbolControls();
    m_symbolInput = symbolControls.symbolInput;
    m_subscribeButton = symbolControls.subscribeButton;
    
    // Add status bar to main window
    statusBar()->addPermanentWidget(m_statusBar);
    statusBar()->setStyleSheet("QStatusBar { background-color: #1e1e1e; border-top: 1px solid #333; }");
    
    // Connect symbol changes to docks
    connect(this, &MainWindowGPU::symbolChanged, m_secDock, &SecFilingDock::onSymbolChanged);
    connect(this, &MainWindowGPU::symbolChanged, m_marketDataDock, &MarketDataPanel::onSymbolChanged);
    
    setUpdatesEnabled(true);
}

// QML loading and GPU verification moved to QmlSceneController

// Styles are now handled by individual dock widgets

void MainWindowGPU::setWindowProperties() {
    setWindowTitle("Sentinel - GPU Trading Terminal");
    // Set window state to maximized (will be applied when window is shown)
    setWindowState(Qt::WindowMaximized);
}

void MainWindowGPU::setupConnections() {
    LOG_SCOPE("Setting up connections");
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    connectMarketDataSignals();
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty() || !symbol.contains('-')) {
        sLog_Warning("Invalid symbol: " << symbol);
        QMessageBox::warning(this, "Invalid Input", "Enter a valid symbol like BTC-USD.");
        return;
    }
    
    sLog_App("Subscribing to: " << symbol);
    m_qmlController->updateSymbolInContext(symbol);
    propagateSymbolChange(symbol);
    if (m_dataSource) {
        m_dataSource->subscribe(symbol);
    }
}

void MainWindowGPU::propagateSymbolChange(const QString& symbol) {
    emit symbolChanged(symbol);
}

void MainWindowGPU::closeEvent(QCloseEvent* event) {
    // Auto-save current layout so it restores on next launch
    sLog_App("Saving session layout on close");
    m_layoutOrchestrator->saveLayout("_last_session");
    QMainWindow::closeEvent(event);
}

void MainWindowGPU::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    
    sLog_App("=== showEvent triggered ===");
    sLog_App("  Window size: " << width() << "x" << height());
    
    // First show: maximize window, then restore/arrange layout AFTER maximization completes
    if (m_firstShow) {
        m_firstShow = false;
        
        // Step 1: Maximize the window first
        if (windowState() != Qt::WindowMaximized) {
            showMaximized();
        }
        
        // Step 2: Restore or arrange layout AFTER window has fully maximized
        QTimer::singleShot(50, this, [this]() {
            sLog_App("=== Restoring layout after maximize ===");
            sLog_App("  Window size: " << width() << "x" << height());
            
            // Try to restore last session layout, fall back to default
            if (m_layoutOrchestrator->restoreLayout(getDockWidgets(), "_last_session")) {
                sLog_App("Restored last session layout");
            } else {
                sLog_App("No saved session, using default layout");
                m_layoutOrchestrator->arrangeDefaultLayout(getDockWidgets());
            }
        });
    }
}

void MainWindowGPU::setupMenuBar() {
    MenuBuilder::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.marketDataDock = m_marketDataDock;
    docks.secDock = m_secDock;
    docks.copenetDock = m_copenetDock;
    docks.aiCommentaryDock = m_aiCommentaryDock;
    
    MenuBuilder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    callbacks.openSecFilingViewer = [this]() { onOpenSecFilingViewer(); };
    callbacks.openMarketDataPanel = [this]() { onOpenMarketDataPanel(); };
    
    m_menuBuilder->buildMenus(docks, callbacks);
}

void MainWindowGPU::setupShortcuts() {
    ShortcutBinder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    
    ShortcutBinder::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.marketDataDock = m_marketDataDock;
    docks.secDock = m_secDock;
    
    m_shortcutBinder->bindShortcuts(callbacks, docks);
}

// Symbol context updates moved to QmlSceneController

void MainWindowGPU::connectMarketDataSignals() {
    LOG_SCOPE("Connecting MarketData signals");
    
    auto unifiedGridRenderer = m_qmlController->getUnifiedGridRenderer();
    if (!m_dataSource || !unifiedGridRenderer) {
        sLog_Error("Cannot connect signals: Missing components");
        return;
    }
    
    unifiedGridRenderer->setDataSource(m_dataSource.get());

    auto dataProcessor = unifiedGridRenderer->getDataProcessor();
    if (dataProcessor) {
        connect(m_dataSource.get(), &IGridDataSource::liveOrderBookUpdated,
                dataProcessor, &DataProcessor::onLiveOrderBookUpdated, Qt::QueuedConnection);
    }
    
    // Trade connection (simplified, assume UnifiedGridRenderer has a slot)
    connect(m_dataSource.get(), &IGridDataSource::tradeReceived,
            unifiedGridRenderer, &UnifiedGridRenderer::onTradeReceived, Qt::QueuedConnection);
    
    connect(m_dataSource.get(), &IGridDataSource::connectionStatusChanged,
            this, &MainWindowGPU::onConnectionStatusChanged);

    // Surface DataSource errors (e.g., WS/TLS/DNS issues) into the app log
    connect(m_dataSource.get(), &IGridDataSource::errorOccurred,
            this, [](const QString& error) {
                sLog_Error("DataSource error: " << error);
            });
}

void MainWindowGPU::onConnectionStatusChanged(bool connected) {  // Extracted for clarity
    // Update bottom status bar
    if (m_statusBar) {
        m_statusBar->setConnectionStatus(connected);
    }
    
    if (m_subscribeButton) {
        m_subscribeButton->setText(connected ? "Subscribe" : "Connect");
        m_subscribeButton->setEnabled(true);
    }
}

bool MainWindowGPU::validateComponents() {
    if (!m_qmlController || !m_qmlController->isValid()) return false;
    if (!m_qmlController->getUnifiedGridRenderer()) return false;
    if (!m_marketDataCore) return false;
    return true;
}

// Layout arrangement moved to LayoutOrchestrator

void MainWindowGPU::resetLayoutToDefault() {
    m_layoutOrchestrator->resetLayoutToDefault(getDockWidgets());
}

// UnifiedGridRenderer access moved to QmlSceneController

void MainWindowGPU::onSaveLayout() {
    bool ok;
    QString name = QInputDialog::getText(this, "Save Layout", "Layout name:", 
                                        QLineEdit::Normal, "", &ok);
    if (ok && !name.isEmpty()) {
        m_layoutOrchestrator->saveLayout(name);
        sLog_App("Layout saved: " << name);
    }
}

void MainWindowGPU::onRestoreLayout() {
    QStringList layouts = LayoutManager::availableLayouts();
    if (layouts.isEmpty()) {
        QMessageBox::information(this, "No Layouts", "No saved layouts found.");
        return;
    }
    
    bool ok;
    QString selected = QInputDialog::getItem(this, "Restore Layout", "Select layout:",
                                            layouts, 0, false, &ok);
    if (ok && !selected.isEmpty()) {
        if (m_layoutOrchestrator->restoreLayout(getDockWidgets(), selected)) {
            sLog_App("Layout restored: " << selected);
        } else {
            QMessageBox::warning(this, "Restore Failed", 
                                "Failed to restore layout. Using default arrangement.");
        }
    }
}

void MainWindowGPU::onResetLayout() {
    resetLayoutToDefault();
    sLog_App("Layout reset to default");
}

void MainWindowGPU::onOpenSecFilingViewer() {
    if (m_secDock && !m_secDock->isVisible()) {
        m_secDock->show();
        m_secDock->raise();
    }
}

void MainWindowGPU::onOpenMarketDataPanel() {
    if (m_marketDataDock && !m_marketDataDock->isVisible()) {
        m_marketDataDock->show();
        m_marketDataDock->raise();
    }
}

LayoutOrchestrator::DockWidgets MainWindowGPU::getDockWidgets() const {
    LayoutOrchestrator::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.marketDataDock = m_marketDataDock;
    docks.secDock = m_secDock;
    docks.copenetDock = m_copenetDock;
    docks.aiCommentaryDock = m_aiCommentaryDock;
    return docks;
}
