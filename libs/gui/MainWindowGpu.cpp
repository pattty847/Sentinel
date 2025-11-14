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
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QShortcut>
#include <QInputDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QStatusBar>  // For status bar integration
#include "ChartModeController.h"
#include "MainWindowGpu.h"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"
#include "SentinelLogging.hpp"
#include "widgets/DockablePanel.hpp"
#include "widgets/HeatmapDock.hpp"
#include "widgets/StatusDock.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/MarketDataPanel.hpp"
#include "widgets/SecFilingDock.hpp"
#include "widgets/CopenetFeedDock.hpp"
#include "widgets/AICommentaryFeedDock.hpp"
#include "widgets/LayoutManager.hpp"
#include "widgets/ServiceLocator.hpp"
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

MainWindowGPU::MainWindowGPU(QWidget* parent) : QMainWindow(parent) {
    LOG_SCOPE("MainWindowGPU construction");
    
    initializeDataComponents();
    
    // Register services with ServiceLocator
    ServiceLocator::registerMarketDataCore(m_marketDataCore.get());
    ServiceLocator::registerDataCache(m_dataCache.get());
    
    setupUI();
    setupMenuBar();
    setupShortcuts();
    setupConnections();
    setWindowProperties();
    
    // Restore default layout if it exists
    if (!LayoutManager::restoreLayout(this, LayoutManager::defaultLayoutName())) {
        sLog_App("No saved layout found, using default arrangement");
    }
    
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
}

void MainWindowGPU::initializeDataComponents() {
    LOG_SCOPE("Initializing data components");
    
    // Load config (example: extract to separate class if complex)
    QSettings config("config.ini", QSettings::IniFormat);
    QString defaultKeyFile = config.value("auth/keyFile", "key.json").toString();
    
    m_authenticator = std::make_unique<Authenticator>(defaultKeyFile.toStdString());
    m_dataCache = std::make_unique<DataCache>();

    m_marketDataCore = std::make_unique<MarketDataCore>(*m_authenticator, *m_dataCache);
    m_marketDataCore->start();
}

void MainWindowGPU::setupUI() {
    // TODO: Modularize this into separate methods for clarity
    LOG_SCOPE("Setting up UI");
    
    // Enable docking features
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AnimatedDocks);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    
    // Prevent repaint storms during setup
    setUpdatesEnabled(false);
    
    // Create StatusDock as central widget (minimal, just a placeholder)
    m_statusDock = new StatusDock(this);
    setCentralWidget(m_statusDock);
    
    // Create bottom status bar (dock-like, Windows taskbar style)
    m_statusBar = new StatusBar(this);
    // Add our custom status bar widget to Qt's status bar
    statusBar()->addPermanentWidget(m_statusBar);
    statusBar()->setStyleSheet("QStatusBar { background-color: #1e1e1e; border-top: 1px solid #333; }");
    
    // Create HeatmapDock
    m_heatmapDock = new HeatmapDock(this);
    m_qquickView = m_heatmapDock->qquickView();
    m_qmlContainer = m_heatmapDock->qmlContainer();
    
    // Load QML source and configure
    loadQmlSource();
    verifyGpuAcceleration();
    
    QQmlContext* context = m_qquickView->rootContext();
    m_modeController = new ChartModeController(this);
    context->setContextProperty("chartModeController", m_modeController);
    updateSymbolInContext("BTC-USD");  // Centralized
    
    // Create all docks first
    m_marketDataDock = new MarketDataPanel(this);
    m_secDock = new SecFilingDock(this);
    m_copenetDock = new CopenetFeedDock(this);
    m_aiCommentaryDock = new AICommentaryFeedDock(this);
    
    // Get references to embedded symbol controls from HeatmapDock
    m_symbolInput = m_heatmapDock->symbolInput();
    m_subscribeButton = m_heatmapDock->subscribeButton();
    
    // Set minimum sizes for better default layout
    m_heatmapDock->setMinimumWidth(800);  // Heatmap needs space
    m_heatmapDock->setMinimumHeight(600);  // Includes embedded symbol bar
    m_secDock->setMinimumWidth(300);      // SEC panel can be narrower
    m_marketDataDock->setMinimumWidth(350);  // Market data wider to fill gap
    
    // Arrange docks for optimal layout: Heatmap prominent and centered
    // Strategy: Heatmap gets RightDockWidgetArea (large, takes most space)
    //           Symbol control pinned directly below heatmap
    //           Side panels on left, feeds at bottom
    arrangeDefaultLayout();
    
    // Connect symbol changes to docks
    connect(this, &MainWindowGPU::symbolChanged, m_secDock, &SecFilingDock::onSymbolChanged);
    connect(this, &MainWindowGPU::symbolChanged, m_marketDataDock, &MarketDataPanel::onSymbolChanged);
    
    // Symbol controls are now embedded in HeatmapDock
    setUpdatesEnabled(true);
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
    // Styles are now handled by individual dock widgets
    // Keep this method for any future global styling needs
}

void MainWindowGPU::setWindowProperties() {
    setWindowTitle("Sentinel - GPU Trading Terminal");
    // TODO: Make full screen configurable to system
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
    propagateSymbolChange(symbol);
    if (m_marketDataCore) {
        m_marketDataCore->subscribeToSymbols({symbol.toStdString()});
    }
}

void MainWindowGPU::propagateSymbolChange(const QString& symbol) {
    emit symbolChanged(symbol);
    // Propagate to all dock panels
    for (auto* dock : findChildren<DockablePanel*>()) {
        dock->onSymbolChanged(symbol);
    }
}

void MainWindowGPU::closeEvent(QCloseEvent* event) {
    // Save current layout as default
    LayoutManager::saveLayout(this, LayoutManager::defaultLayoutName());
    QMainWindow::closeEvent(event);
}

void MainWindowGPU::setupMenuBar() {
    QMenuBar* menuBar = this->menuBar();
    
    // View Menu
    QMenu* viewMenu = menuBar->addMenu("&View");
    // Add dock visibility toggles (symbol control is now embedded in heatmap)
    if (m_heatmapDock) {
        viewMenu->addAction(m_heatmapDock->toggleViewAction());
    }
    if (m_marketDataDock) {
        viewMenu->addAction(m_marketDataDock->toggleViewAction());
    }
    if (m_secDock) {
        viewMenu->addAction(m_secDock->toggleViewAction());
    }
    if (m_copenetDock) {
        viewMenu->addAction(m_copenetDock->toggleViewAction());
    }
    if (m_aiCommentaryDock) {
        viewMenu->addAction(m_aiCommentaryDock->toggleViewAction());
    }
    
    // Layouts Menu
    QMenu* layoutsMenu = menuBar->addMenu("&Layouts");
    
    QAction* saveLayoutAction = layoutsMenu->addAction("&Save Current Layout...");
    connect(saveLayoutAction, &QAction::triggered, this, [this]() {
        bool ok;
        QString name = QInputDialog::getText(this, "Save Layout", "Layout name:", 
                                            QLineEdit::Normal, "", &ok);
        if (ok && !name.isEmpty()) {
            LayoutManager::saveLayout(this, name);
            sLog_App("Layout saved: " << name);
        }
    });
    
    QAction* restoreLayoutAction = layoutsMenu->addAction("&Restore Layout...");
    connect(restoreLayoutAction, &QAction::triggered, this, [this]() {
        QStringList layouts = LayoutManager::availableLayouts();
        if (layouts.isEmpty()) {
            QMessageBox::information(this, "No Layouts", "No saved layouts found.");
            return;
        }
        
        bool ok;
        QString selected = QInputDialog::getItem(this, "Restore Layout", "Select layout:",
                                                layouts, 0, false, &ok);
        if (ok && !selected.isEmpty()) {
            if (LayoutManager::restoreLayout(this, selected)) {
                sLog_App("Layout restored: " << selected);
            } else {
                QMessageBox::warning(this, "Restore Failed", 
                                    "Failed to restore layout. Using default arrangement.");
            }
        }
    });
    
    QAction* resetLayoutAction = layoutsMenu->addAction("&Reset to Default Layout");
    connect(resetLayoutAction, &QAction::triggered, this, [this]() {
        LayoutManager::resetToDefault(this);
        sLog_App("Layout reset to default");
    });
    
    layoutsMenu->addSeparator();
    
    // Tools Menu
    QMenu* toolsMenu = menuBar->addMenu("&Tools");
    QAction* openSecAction = toolsMenu->addAction("Open &SEC Filing Viewer");
    connect(openSecAction, &QAction::triggered, this, [this]() {
        if (m_secDock && !m_secDock->isVisible()) {
            m_secDock->show();
            m_secDock->raise();
        }
    });
    
    QAction* openMarketDataAction = toolsMenu->addAction("Open &Market Data Panel");
    connect(openMarketDataAction, &QAction::triggered, this, [this]() {
        if (m_marketDataDock && !m_marketDataDock->isVisible()) {
            m_marketDataDock->show();
            m_marketDataDock->raise();
        }
    });
    
    toolsMenu->addSeparator();
    QAction* settingsAction = toolsMenu->addAction("&Settings...");
    settingsAction->setEnabled(false);  // Placeholder for future
}

void MainWindowGPU::setupShortcuts() {
    // Ctrl+S - Save current layout
    QShortcut* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this]() {
        bool ok;
        QString name = QInputDialog::getText(this, "Save Layout", "Layout name:",
                                             QLineEdit::Normal, "", &ok);
        if (ok && !name.isEmpty()) {
            LayoutManager::saveLayout(this, name);
        }
    });
    
    // Ctrl+L - Restore layout dialog
    QShortcut* loadShortcut = new QShortcut(QKeySequence("Ctrl+L"), this);
    connect(loadShortcut, &QShortcut::activated, this, [this]() {
        QStringList layouts = LayoutManager::availableLayouts();
        if (layouts.isEmpty()) {
            QMessageBox::information(this, "No Layouts", "No saved layouts found.");
            return;
        }
        bool ok;
        QString selected = QInputDialog::getItem(this, "Restore Layout", "Select layout:",
                                                 layouts, 0, false, &ok);
        if (ok && !selected.isEmpty()) {
            LayoutManager::restoreLayout(this, selected);
        }
    });
    
    // Ctrl+R - Reset to default layout
    QShortcut* resetShortcut = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(resetShortcut, &QShortcut::activated, this, [this]() {
        LayoutManager::resetToDefault(this);
    });
    
    // F1-F3 - Toggle docks
    QShortcut* f1Shortcut = new QShortcut(QKeySequence("F1"), this);
    connect(f1Shortcut, &QShortcut::activated, this, [this]() {
        if (m_heatmapDock) m_heatmapDock->setVisible(!m_heatmapDock->isVisible());
    });
    
    QShortcut* f2Shortcut = new QShortcut(QKeySequence("F2"), this);
    connect(f2Shortcut, &QShortcut::activated, this, [this]() {
        if (m_marketDataDock) m_marketDataDock->setVisible(!m_marketDataDock->isVisible());
    });
    
    QShortcut* f3Shortcut = new QShortcut(QKeySequence("F3"), this);
    connect(f3Shortcut, &QShortcut::activated, this, [this]() {
        if (m_secDock) m_secDock->setVisible(!m_secDock->isVisible());
    });
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
    if (m_qquickView->status() != QQuickView::Ready) return false;
    if (!getUnifiedGridRenderer()) return false;
    if (!m_marketDataCore) return false;
    return true;
}

void MainWindowGPU::arrangeDefaultLayout() {
    // Prevent repaint storms during layout changes
    setUpdatesEnabled(false);
    
    // Remove all docks from their current positions first
    if (m_heatmapDock && m_heatmapDock->parent() == this) {
        removeDockWidget(m_heatmapDock);
    }
    if (m_secDock && m_secDock->parent() == this) {
        removeDockWidget(m_secDock);
    }
    if (m_marketDataDock && m_marketDataDock->parent() == this) {
        removeDockWidget(m_marketDataDock);
    }
    if (m_copenetDock && m_copenetDock->parent() == this) {
        removeDockWidget(m_copenetDock);
    }
    if (m_aiCommentaryDock && m_aiCommentaryDock->parent() == this) {
        removeDockWidget(m_aiCommentaryDock);
    }
    
    // Right: Heatmap (includes embedded symbol control bar at bottom)
    addDockWidget(Qt::RightDockWidgetArea, m_heatmapDock);
    
    // Left: SEC Filing Viewer and Market Data (tabbed together, narrow sidebar)
    addDockWidget(Qt::LeftDockWidgetArea, m_secDock);
    addDockWidget(Qt::LeftDockWidgetArea, m_marketDataDock);
    tabifyDockWidget(m_secDock, m_marketDataDock);  // Tab them together
    
    // Bottom: Commentary feeds (small height, split horizontally)
    addDockWidget(Qt::BottomDockWidgetArea, m_copenetDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_aiCommentaryDock);
    tabifyDockWidget(m_copenetDock, m_aiCommentaryDock);
    
    // Set relative sizes for bottom feeds (they share the bottom area equally)
    resizeDocks({m_copenetDock, m_aiCommentaryDock}, {1, 1}, Qt::Horizontal);
    
    // Ensure all docks are visible
    if (m_heatmapDock) m_heatmapDock->show();
    if (m_secDock) m_secDock->show();
    if (m_marketDataDock) m_marketDataDock->show();
    if (m_copenetDock) m_copenetDock->show();
    if (m_aiCommentaryDock) m_aiCommentaryDock->show();
    
    setUpdatesEnabled(true);
}

void MainWindowGPU::resetLayoutToDefault() {
    sLog_App("Resetting layout to default");
    arrangeDefaultLayout();
    // Clear saved layout so next time it uses this fresh default
    LayoutManager::deleteLayout(LayoutManager::defaultLayoutName());
}

UnifiedGridRenderer* MainWindowGPU::getUnifiedGridRenderer() const {
    return m_qquickView->rootObject() ? m_qquickView->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer") : nullptr;
}
