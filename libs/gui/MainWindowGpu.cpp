/*
Sentinel — MainWindowGpu
Role: Implements UI setup and the core data-to-rendering pipeline connection logic.
Inputs/Outputs: Connects remote data source signals to UnifiedGridRenderer slots.
Threading: Runs on the main GUI thread, using QueuedConnections for thread safety.
Performance: Connection logic is part of the user-initiated subscription setup.
Integration: Wires the remote data source to the QML renderer.
Observability: Detailed logging of UI/QML initialization and data pipeline status via sLog_App/sLog_Data.
Related: MainWindowGpu.h, UnifiedGridRenderer.h.
Assumptions: Remote data source connects before subscribe() is called.
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
#include "widgets/SecFilingDock.hpp"
#include "widgets/LayoutManager.hpp"
#include "widgets/ServiceLocator.hpp"
#include "mainwindow/DockFactory.h"
#include "mainwindow/QmlSceneController.h"
#include "mainwindow/LayoutOrchestrator.h"
#include "mainwindow/MenuBuilder.h"
#include "mainwindow/ShortcutBinder.h"
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
#include <QScreen>
#include <QApplication>

MainWindowGPU::MainWindowGPU(QWidget* parent) : QMainWindow(parent) {
    // 1) Initialize data source (remote-only)
    auto remote = std::make_unique<RemoteGridDataSource>("127.0.0.1", "8080");
    remote->connectToServer();
    m_dataSource = std::move(remote);
    ServiceLocator::registerDataSource(m_dataSource.get());
    
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
}

MainWindowGPU::~MainWindowGPU() {
}

void MainWindowGPU::setupUI() {
    
    // Prevent repaint storms during setup
    setUpdatesEnabled(false);
    
    // Create docks via DockFactory
    DockFactory dockFactory(this);
    auto docks = dockFactory.createDocks();
    m_heatmapDock = docks.heatmapDock;
    m_statusBar = docks.statusBar;
    if (m_heatmapDock) {
        m_symbolInput = m_heatmapDock->symbolInput();
        m_subscribeButton = m_heatmapDock->subscribeButton();
    }
    m_secDock = docks.secDock;
    m_copenetDock = docks.copenetDock;
    m_aiCommentaryDock = docks.aiCommentaryDock;
    m_labDock = docks.labDock;
    
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
    connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
    connectMarketDataSignals();
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty() || !symbol.contains('-')) {
        QMessageBox::warning(this, "Invalid Input", "Enter a valid symbol like BTC-USD.");
        return;
    }

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
    m_layoutOrchestrator->saveLayout("_last_session");
    QMainWindow::closeEvent(event);
}

void MainWindowGPU::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (m_firstShow) {
        m_firstShow = false;

        if (windowState() != Qt::WindowMaximized) {
            showMaximized();
        }

        QTimer::singleShot(50, this, [this]() {
            if (!m_layoutOrchestrator->restoreLayout(getDockWidgets(), "_last_session")) {
                m_layoutOrchestrator->arrangeDefaultLayout(getDockWidgets());
            }
        });
    }
}

void MainWindowGPU::setupMenuBar() {
    MenuBuilder::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.secDock = m_secDock;
    docks.copenetDock = m_copenetDock;
    docks.aiCommentaryDock = m_aiCommentaryDock;
    docks.labDock = m_labDock;
    
    MenuBuilder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    callbacks.openSecFilingViewer = [this]() { onOpenSecFilingViewer(); };

    // Set heatmap dock for debug menu access
    m_menuBuilder->setHeatmapDock(m_heatmapDock);

    m_menuBuilder->buildMenus(docks, callbacks);
}

void MainWindowGPU::setupShortcuts() {
    ShortcutBinder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    
    ShortcutBinder::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.secDock = m_secDock;
    
    m_shortcutBinder->bindShortcuts(callbacks, docks);
}

// Symbol context updates moved to QmlSceneController

void MainWindowGPU::connectMarketDataSignals() {
    auto unifiedGridRenderer = m_qmlController->getUnifiedGridRenderer();
    if (!m_dataSource || !unifiedGridRenderer) {
        sLog_Error("Cannot connect signals: Missing components");
        return;
    }
    
    auto dataProcessor = unifiedGridRenderer->getDataProcessor();
    if (dataProcessor) {
        connect(m_dataSource.get(), &IGridDataSource::heatmapSliceReceived,
                dataProcessor, &DataProcessor::onHeatmapSliceReceived, Qt::QueuedConnection);
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
    if (!m_dataSource) return false;
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
        if (!m_layoutOrchestrator->restoreLayout(getDockWidgets(), selected)) {
            QMessageBox::warning(this, "Restore Failed",
                                "Failed to restore layout. Using default arrangement.");
        }
    }
}

void MainWindowGPU::onResetLayout() {
    resetLayoutToDefault();
}

void MainWindowGPU::onOpenSecFilingViewer() {
    if (m_secDock && !m_secDock->isVisible()) {
        m_secDock->show();
        m_secDock->raise();
    }
}

LayoutOrchestrator::DockWidgets MainWindowGPU::getDockWidgets() const {
    LayoutOrchestrator::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.secDock = m_secDock;
    docks.copenetDock = m_copenetDock;
    docks.aiCommentaryDock = m_aiCommentaryDock;
    docks.labDock = m_labDock;
    return docks;
}
