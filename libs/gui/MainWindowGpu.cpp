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
#include <QElapsedTimer>
#include <QThread>
#include <QDateTime>
#include <QUuid>
#include "ChartModeController.h"
#include "MainWindowGpu.h"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"
#include "SentinelLogging.hpp"
#include "widgets/ChartDock.hpp"
#include "widgets/LabDock.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/SecFilingDock.hpp"
#include "widgets/ScreenerDock.hpp"
#include "widgets/CopenetFeedDock.hpp"
#include "widgets/AICommentaryFeedDock.hpp"
#include "widgets/TopToolbar.hpp"
#include "widgets/HeatmapSettingsDialog.hpp"
#include "widgets/WatchlistDock.hpp"
#include "widgets/StockChartDock.hpp"
#include "widgets/OrderBookDock.hpp"
#include "widgets/PaperTradingDock.hpp"
#include "widgets/FontSettingsDialog.hpp"
#include "render/AlgoOverlayRenderer.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "widgets/LayoutManager.hpp"
#include "widgets/ServiceLocator.hpp"
#include "PerformanceMonitor.hpp"
#include "mainwindow/DockFactory.h"
#include "mainwindow/QmlSceneController.h"
#include "mainwindow/LayoutOrchestrator.h"
#include "mainwindow/MenuBuilder.h"
#include "mainwindow/ShortcutBinder.h"
#include "mainwindow/GuiApiServer.h"
#include "datasources/RemoteGridDataSource.hpp"
#include "TradeInputManager.hpp"
#include "config/GuiConfigStore.hpp"
#include "themes/ThemeBridge.hpp"
#include "themes/ThemeManager.hpp"
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
#include <QTabWidget>
#include <QtGlobal>
#include <algorithm>
#include <unordered_map>

namespace {
int timeframeMsFromLabel(const QString& label) {
    static const std::unordered_map<std::string, int> map{
        {"1s", 1000},
        {"1m", 60000},
        {"5m", 300000},
        {"15m", 900000},
        {"1h", 3600000},
        {"4h", 14400000},
        {"1D", 86400000},
    };
    auto it = map.find(label.toStdString());
    if (it == map.end()) {
        return 0;
    }
    return it->second;
}

bool chartDebugEnabled() {
    static const bool enabled = qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG");
    return enabled;
}
}

MainWindowGPU::MainWindowGPU(QWidget* parent) : QMainWindow(parent) {
    const auto& clientConfig = GuiConfigStore::instance().clientConfig();
    auto remote = std::make_unique<RemoteGridDataSource>(
        QString::fromStdString(clientConfig.server.host),
        QString::fromStdString(clientConfig.server.port),
        QString::fromStdString(clientConfig.server.caFile));
    remote->connectToServer();
    m_dataSource = std::move(remote);
    ServiceLocator::registerDataSource(m_dataSource.get());
    setupUI();
    if (m_qquickView) {
        m_qmlController = std::make_unique<QmlSceneController>(m_qquickView);
    }
    if (m_qmlController) {
        m_themeBridge = new ThemeBridge(this);
        m_themeBridge->applyTheme(ThemeManager::instance().currentTheme());
        m_qmlController->setThemeBridge(m_themeBridge);
        m_qmlController->setDataSource(m_dataSource.get());
        m_qmlController->loadQmlSource();
        m_qmlController->verifyGpuAcceleration();
    }

    auto* configStore = &GuiConfigStore::instance();
    connect(configStore, &GuiConfigStore::serverConfigUpdated, this, [this](const ServerConfig& config) {
        if (m_qmlController) {
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->applyServerConfig(config);
                if (m_heatmapDock && m_heatmapDock->toolbar()) {
                    m_heatmapDock->toolbar()->setTimeframeMs(renderer->getCurrentTimeframe());
                }
            }
        }
        if (!config.defaultSymbols.empty() && !m_userSubscribed) {
            const QString defaultSymbol = QString::fromStdString(config.defaultSymbols.front());
            if (m_symbolInput) {
                m_symbolInput->setText(defaultSymbol);
            }
            if (m_qmlController) {
                m_qmlController->updateSymbolInContext(defaultSymbol);
            }
            m_currentSymbol = defaultSymbol;
        }
    });

    // Attach PerformanceMonitor to QML window for FPS tracking
    if (m_qquickView) {
        PerformanceMonitor::instance().attachToWindow(m_qquickView);
        sLog_App("PerformanceMonitor attached to QML window");
    }
    if (m_statusBar) {
        auto& perfMon = PerformanceMonitor::instance();
        connect(&perfMon, &PerformanceMonitor::fpsChanged, m_statusBar, &StatusBar::setFps);
        connect(&perfMon, &PerformanceMonitor::cpuUsageChanged, m_statusBar, &StatusBar::setCpuUsage);
        connect(&perfMon, &PerformanceMonitor::gpuUsageChanged, m_statusBar, &StatusBar::setGpuUsage);
        connect(&perfMon, &PerformanceMonitor::latencyChanged, m_statusBar, &StatusBar::setLatency);
        if (auto* remote = dynamic_cast<RemoteGridDataSource*>(m_dataSource.get())) {
            connect(remote->streamClient(), &SentinelStreamClient::coinbaseLatencyReceived,
                    m_statusBar, &StatusBar::setCoinbaseLatency, Qt::QueuedConnection);
        }
        sLog_App("StatusBar connected to PerformanceMonitor (FPS, CPU, GPU, Latency)");
    }
    
    m_modeController = new ChartModeController(this);
    if (m_qmlController) {
        m_qmlController->setChartModeController(m_modeController);
        const QString defaultSymbol = QStringLiteral("BTC-USD");
        m_qmlController->updateSymbolInContext(defaultSymbol);  // Default symbol
        m_currentSymbol = defaultSymbol;
    }
    m_modeController->setPrimaryField(ChartModeController::PrimaryField::Heatmap);
    m_modeController->setCandlesEnabled(true);
    m_layoutOrchestrator = std::make_unique<LayoutOrchestrator>(this);
    // Defer arrangeDefaultLayout() until after show: resizeDocks() fails at default 640x480.
    m_menuBuilder = std::make_unique<MenuBuilder>(menuBar());
    m_shortcutBinder = std::make_unique<ShortcutBinder>(this);
    setupMenuBar();
    setupShortcuts();
    
    setupConnections();
    setWindowProperties();
    setupGuiApiServer();
    
    if (!validateComponents()) {
        sLog_Error("Component validation failed - app may not function correctly");
        QMessageBox::critical(this, "Initialization Error", "Failed to initialize core components. Check logs.");
    }
}

MainWindowGPU::~MainWindowGPU() {
}

void MainWindowGPU::setupUI() {
    setUpdatesEnabled(false);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
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
    m_watchlistDock = docks.watchlistDock;
    m_screenerDock = docks.screenerDock;
    m_stockChartDock = docks.stockChartDock;
    m_orderBookDock = docks.orderBookDock;
    m_paperTradingDock = new PaperTradingDock(this);
    m_paperTradingDock->setDataSource(m_dataSource.get());
    if (m_screenerDock) {
        if (auto* remote = dynamic_cast<RemoteGridDataSource*>(m_dataSource.get())) {
            m_screenerDock->setStreamClient(remote->streamClient());
        }
        connect(m_screenerDock, &ScreenerDock::rowSelected,
                this, &MainWindowGPU::onAssetSymbolSelected);
    }
    if (m_watchlistDock) {
        connect(m_watchlistDock, &WatchlistDock::symbolSelected,
                this, &MainWindowGPU::onAssetSymbolSelected);
    }
    
    m_qquickView = m_heatmapDock->qquickView();
    m_qmlContainer = m_heatmapDock->qmlContainer();
    if (m_qquickView) {
    }
    auto symbolControls = dockFactory.getSymbolControls();
    m_symbolInput = symbolControls.symbolInput;
    m_subscribeButton = symbolControls.subscribeButton;
    
    // Add status bar to main window
    statusBar()->addPermanentWidget(m_statusBar);
    statusBar()->setStyleSheet("QStatusBar { background-color: #1e1e1e; border-top: 1px solid #333; }");
    connect(this, &MainWindowGPU::symbolChanged, m_secDock, &SecFilingDock::onSymbolChanged);
    if (m_orderBookDock) {
        connect(this, &MainWindowGPU::symbolChanged, m_orderBookDock, &OrderBookDock::onSymbolChanged);
    }
    if (m_paperTradingDock) {
        connect(this, &MainWindowGPU::symbolChanged, m_paperTradingDock, &PaperTradingDock::setSymbol, Qt::QueuedConnection);
    }
    if (m_heatmapDock && m_heatmapDock->toolbar()) {
        connect(m_heatmapDock->toolbar(), &TopToolbar::primaryFieldRequested, this, [this](int field) {
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Primary field request: %1").arg(field));
            }
            if (m_modeController) {
                m_modeController->setPrimaryField(field);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::heatmapToggled, this, [this](bool enabled) {
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Toolbar heatmap toggled: %1").arg(enabled));
            }
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setHeatmapLayerEnabled(enabled);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::footprintToggled, this, [this](bool enabled) {
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Toolbar footprint toggled: %1").arg(enabled));
            }
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setFootprintLayerEnabled(enabled);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::tpoToggled, this, [this](bool enabled) {
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Toolbar TPO toggled: %1").arg(enabled));
            }
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setTpoLayerEnabled(enabled);
            }
            if (enabled && m_connected && m_userSubscribed) {
                requestTpoHistoryForSymbol(m_currentSymbol);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::volumeProfileToggled, this, [this](bool enabled) {
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Toolbar volume profile toggled: %1").arg(enabled));
            }
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setVolumeProfileLayerEnabled(enabled);
            }
            if (enabled && m_connected && m_userSubscribed) {
                requestTpoHistoryForSymbol(m_currentSymbol);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::candlesToggled, this, [this](bool enabled) {
            if (m_modeController) {
                m_modeController->setCandlesEnabled(enabled);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::liquidityThresholdChanged, this, [this](double value) {
            if (!m_qmlController) return;
            auto* renderer = m_qmlController->getUnifiedGridRenderer();
            if (renderer) {
                renderer->setProperty("heatmapLiquidityThreshold", value);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::liquidityLabelModeChanged, this, [this](int mode) {
            if (!m_qmlController) return;
            auto* renderer = m_qmlController->getUnifiedGridRenderer();
            if (renderer) {
                renderer->setProperty("liquidityLabelMode", mode);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::subscribeRequested, this, &MainWindowGPU::onSubscribe);
        connect(m_heatmapDock->toolbar(), &TopToolbar::settingsRequested, this, [this]() {
            if (!m_qmlController) return;
            auto* renderer = m_qmlController->getUnifiedGridRenderer();
            if (!renderer) return;
            if (!m_heatmapSettingsDialog) {
                m_heatmapSettingsDialog = new HeatmapSettingsDialog(renderer, this);
            } else {
                m_heatmapSettingsDialog->setRenderer(renderer);
            }
            m_heatmapSettingsDialog->show();
            m_heatmapSettingsDialog->raise();
            m_heatmapSettingsDialog->activateWindow();
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::timeframeSelected, this, [this](const QString& label) {
            if (!m_qmlController) {
                return;
            }
            auto* renderer = m_qmlController->getUnifiedGridRenderer();
            if (!renderer) {
                return;
            }
            const int ms = timeframeMsFromLabel(label);
            if (ms <= 0) {
                return;
            }
            if (chartDebugEnabled()) {
                sLog_Debug(QString("Chart TF select: %1 -> %2ms").arg(label).arg(ms));
            }
            renderer->setTimeframe(ms);
            if (m_connected && m_userSubscribed) {
                requestHeatmapHistoryForSymbol(m_currentSymbol);
                requestCandleHistoryForSymbol(m_currentSymbol);
                if (m_modeController && m_modeController->primaryField() == 1) {
                    requestFootprintHistoryForSymbol(m_currentSymbol);
                } else if (m_modeController && (m_modeController->primaryField() == 2 ||
                                                m_modeController->primaryField() == 3)) {
                    requestTpoHistoryForSymbol(m_currentSymbol);
                }
            }
        });
    }
    
    setUpdatesEnabled(true);
}

void MainWindowGPU::setWindowProperties() {
    setWindowTitle("Sentinel - GPU Trading Terminal");
    setWindowState(Qt::WindowMaximized);
}

void MainWindowGPU::setupConnections() {
    if (m_subscribeButton) {
        connect(m_subscribeButton, &QToolButton::clicked, this, &MainWindowGPU::onSubscribe);
    }
    if (m_symbolInput) {
        connect(m_symbolInput, &QLineEdit::returnPressed, this, [this]() {
            if (!m_symbolInput) return;
            m_symbolInput->setText(m_symbolInput->text().trimmed().toUpper());
            onSubscribe();
        });
    }
    connectMarketDataSignals();
    if (m_paperTradingDock) {
        connect(m_dataSource.get(), &IGridDataSource::tradeReceived,
                m_paperTradingDock, &PaperTradingDock::onTradeReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::orderUpdated,
                m_paperTradingDock, &PaperTradingDock::onOrderUpdated, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::positionUpdated,
                m_paperTradingDock, &PaperTradingDock::onPositionUpdated, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::algoOrderEventReceived,
                m_paperTradingDock, &PaperTradingDock::onAlgoOrderEvent, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::pnlSnapshotReceived,
                m_paperTradingDock, &PaperTradingDock::onPnlSnapshot, Qt::QueuedConnection);
    }

    if (m_qquickView && m_qquickView->rootObject()) {
        if (auto* overlay = m_qquickView->rootObject()->findChild<AlgoOverlayRenderer*>("algoOverlayRenderer")) {
            connect(m_dataSource.get(), &IGridDataSource::algoOrderEventReceived,
                    overlay, &AlgoOverlayRenderer::onAlgoOrderEvent, Qt::QueuedConnection);
        }
        if (auto* overlayModel = m_qquickView->rootObject()->findChild<PaperTradeOverlayModel*>("paperTradeOverlayModel")) {
            connect(m_dataSource.get(), &IGridDataSource::tradeReceived,
                    overlayModel, &PaperTradeOverlayModel::onTradeReceived, Qt::QueuedConnection);
            connect(m_dataSource.get(), &IGridDataSource::orderUpdated,
                    overlayModel, &PaperTradeOverlayModel::onOrderUpdated, Qt::QueuedConnection);
            connect(m_dataSource.get(), &IGridDataSource::positionUpdated,
                    overlayModel, &PaperTradeOverlayModel::onPositionUpdated, Qt::QueuedConnection);
            connect(m_dataSource.get(), &IGridDataSource::riskOrderUpdated,
                    overlayModel, &PaperTradeOverlayModel::onRiskOrderUpdated, Qt::QueuedConnection);
            connect(overlayModel, &PaperTradeOverlayModel::applyAttachedRiskRequested,
                    this,
                    [this](bool hasTakeProfit, double takeProfitPrice, bool hasStopLoss, double stopLossPrice) {
                        if (!m_dataSource || m_currentSymbol.isEmpty()) {
                            return;
                        }
                        trading::TradeCommand cmd;
                        cmd.commandId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                        cmd.action = trading::TradeAction::SetAttachedRisk;
                        cmd.symbol = m_currentSymbol.toStdString();
                        cmd.timestamp = QDateTime::currentMSecsSinceEpoch();
                        cmd.hasTakeProfit = hasTakeProfit;
                        cmd.takeProfitPrice = takeProfitPrice;
                        cmd.hasStopLoss = hasStopLoss;
                        cmd.stopLossPrice = stopLossPrice;
                        m_dataSource->sendTradeCommand(cmd);
                    },
                    Qt::QueuedConnection);
        }
    }
}

void MainWindowGPU::setupGuiApiServer() {
    const auto& clientConfig = GuiConfigStore::instance().clientConfig();
    const int defaultPort = clientConfig.gui.apiPort;
    int port = defaultPort;
    if (qEnvironmentVariableIsSet("SENTINEL_GUI_API_PORT")) {
        bool ok = false;
        const int envPort = qEnvironmentVariableIntValue("SENTINEL_GUI_API_PORT", &ok);
        if (ok) {
            port = envPort;
        } else {
            sLog_Error("Invalid SENTINEL_GUI_API_PORT value; using default " << defaultPort);
        }
    }

    if (port == 0) {
        sLog_App("GUI API disabled (api_port=0)");
        return;
    }
    if (port < 0 || port > 65535) {
        sLog_Error("GUI API port out of range; using default " << defaultPort);
        port = defaultPort;
    }

    QString screenshotDir = QString::fromStdString(clientConfig.gui.screenshotDir);
    if (qEnvironmentVariableIsSet("SENTINEL_GUI_SCREENSHOT_DIR")) {
        const QString envDir = qEnvironmentVariable("SENTINEL_GUI_SCREENSHOT_DIR");
        if (!envDir.isEmpty()) {
            screenshotDir = envDir;
        }
    }
    if (screenshotDir.isEmpty()) {
        screenshotDir = QDir::currentPath() + "/screenshots";
    }

    m_guiApiServer = std::make_unique<GuiApiServer>(this,
                                                    m_heatmapDock ? m_heatmapDock->qquickView() : nullptr,
                                                    m_labDock ? m_labDock->qquickView() : nullptr,
                                                    this);
    if (!m_guiApiServer->start(static_cast<quint16>(port), screenshotDir)) {
        sLog_Error("GUI API failed to bind on port " << port << ": " << m_guiApiServer->errorString());
    }
}

void MainWindowGPU::onAssetSymbolSelected(const QString& symbol, const QString& assetType) {
    if (assetType == QLatin1String("crypto") && m_symbolInput) {
        m_symbolInput->setText(symbol);
        onSubscribe();
    } else if (assetType == QLatin1String("stock") && m_stockChartDock) {
        m_stockChartDock->show();
        m_stockChartDock->raise();
        m_stockChartDock->loadSymbol(symbol);
    }
}

void MainWindowGPU::onSubscribe() {
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty() || !symbol.contains('-')) {
        QMessageBox::warning(this, "Invalid Input", "Enter a valid symbol like BTC-USD.");
        return;
    }

    m_userSubscribed = true;
    if (m_qmlController) {
        m_qmlController->updateSymbolInContext(symbol);
    }
    propagateSymbolChange(symbol);
    if (m_dataSource) {
        m_dataSource->subscribe(symbol);
    }
    if (m_connected) {
        requestHeatmapHistoryForSymbol(symbol);
        requestFootprintHistoryForSymbol(symbol);
        requestTpoHistoryForSymbol(symbol);
        requestCandleHistoryForSymbol(symbol);
    }
}

void MainWindowGPU::propagateSymbolChange(const QString& symbol) {
    m_currentSymbol = symbol;
    emit symbolChanged(symbol);
}

void MainWindowGPU::requestHeatmapHistoryForSymbol(const QString& symbol) {
    if (!m_dataSource || symbol.isEmpty()) {
        return;
    }
    int64_t timeframeMs = 0;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            timeframeMs = renderer->getCurrentTimeframe();
        }
    }
    if (timeframeMs <= 0) {
        const auto& serverConfig = GuiConfigStore::instance().serverConfig();
        timeframeMs = static_cast<int64_t>(serverConfig.heatmap.activeTimeframeMs);
        if (timeframeMs <= 0 && !serverConfig.heatmap.timeframesMs.empty()) {
            timeframeMs = serverConfig.heatmap.timeframesMs.front();
        }
    }
    const auto& serverConfig = GuiConfigStore::instance().serverConfig();
    const auto& clientConfig = GuiConfigStore::instance().clientConfig();
    const int requestCount = clientConfig.heatmap.clientCacheColumns;
    const int gridWidth = serverConfig.heatmap.gridWidth;
    const int count = (requestCount > 0) ? requestCount : (gridWidth > 0 ? gridWidth : 5120);
    int64_t tf = (timeframeMs > 0) ? timeframeMs : 1000;
    if (tf <= 0 && !serverConfig.heatmap.timeframesMs.empty()) {
        tf = serverConfig.heatmap.timeframesMs.front();
    }
    if (chartDebugEnabled()) {
        sLog_Debug(QString("Heatmap history request: symbol=%1 tfMs=%2 count=%3")
                   .arg(symbol)
                   .arg(tf)
                   .arg(count));
    }
    m_dataSource->requestHeatmapHistory(symbol, tf, 0, count);
}

void MainWindowGPU::requestFootprintHistoryForSymbol(const QString& symbol) {
    if (!m_dataSource || symbol.isEmpty()) {
        return;
    }
    int64_t timeframeMs = 0;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            timeframeMs = renderer->getCurrentTimeframe();
        }
    }
    if (timeframeMs <= 0) {
        const auto& serverConfig = GuiConfigStore::instance().serverConfig();
        timeframeMs = static_cast<int64_t>(serverConfig.heatmap.activeTimeframeMs);
        if (timeframeMs <= 0 && !serverConfig.heatmap.timeframesMs.empty()) {
            timeframeMs = serverConfig.heatmap.timeframesMs.front();
        }
    }
    if (timeframeMs <= 0) {
        timeframeMs = 1000;
    }
    const auto& serverConfig = GuiConfigStore::instance().serverConfig();
    const int gridWidth = serverConfig.heatmap.gridWidth;
    const int count = (gridWidth > 0) ? std::min(gridWidth, 256) : 256;
    if (chartDebugEnabled()) {
        sLog_Debug(QString("Footprint history request: symbol=%1 tfMs=%2 count=%3")
                   .arg(symbol)
                   .arg(timeframeMs)
                   .arg(count));
    }
    m_dataSource->requestFootprintHistory(symbol, timeframeMs, 0, count);
}

void MainWindowGPU::requestTpoHistoryForSymbol(const QString& symbol) {
    if (!m_dataSource || symbol.isEmpty()) {
        return;
    }
    int64_t timeframeMs = 900000;
    int sessionType = 4;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            timeframeMs = renderer->tpoTimeframeMs();
            sessionType = renderer->tpoSessionType();
        }
    }
    if (timeframeMs != 900000 && timeframeMs != 1800000) {
        timeframeMs = 900000;
    }
    if (sessionType != 4 && sessionType != 5) {
        sessionType = 4;
    }
    const int64_t sessionMs = (sessionType == 5) ? (5LL * 86400000LL) : 86400000LL;
    const int count = static_cast<int>(std::max<int64_t>(1, sessionMs / timeframeMs));
    if (chartDebugEnabled()) {
        sLog_Debug(QString("TPO history request: symbol=%1 tfMs=%2 sessionType=%3 count=%4")
                   .arg(symbol)
                   .arg(timeframeMs)
                   .arg(sessionType)
                   .arg(count));
    }
    m_dataSource->requestTpoHistory(symbol, timeframeMs, sessionType, 0, count);
}

void MainWindowGPU::requestCandleHistoryForSymbol(const QString& symbol) {
    if (!m_dataSource || symbol.isEmpty()) {
        return;
    }
    int64_t timeframeSec = 1;
    int limit = 2000;
    int64_t endTimeSec = 0;
    qint64 viewStart = 0;
    qint64 viewEnd = 0;
    int64_t rendererTfMs = 0;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            rendererTfMs = renderer->getCurrentTimeframe();
            const int64_t timeframeMs = rendererTfMs;
            timeframeSec = std::max<int64_t>(1, (timeframeMs + 999) / 1000);
            viewStart = renderer->getVisibleTimeStart();
            viewEnd = renderer->getVisibleTimeEnd();
            if (viewEnd > viewStart) {
                const int64_t spanSec = std::max<int64_t>(1, (viewEnd - viewStart) / 1000);
                const int64_t spanBars = std::max<int64_t>(1, spanSec / timeframeSec);
                const int64_t cap = (timeframeSec <= 1) ? 10000 : 350;
                limit = static_cast<int>(std::min<int64_t>(cap, spanBars));
                endTimeSec = viewEnd / 1000;
            }
        }
    }
    if (viewEnd <= viewStart) {
        if (!m_qmlController) {
            return;
        }
        auto* renderer = m_qmlController->getUnifiedGridRenderer();
        if (!renderer) {
            return;
        }
        if (!m_candleViewportConn) {
            m_candleViewportConn = connect(renderer, &UnifiedGridRenderer::viewportChanged, this, [this, symbol]() {
                if (!m_qmlController) {
                    return;
                }
                auto* liveRenderer = m_qmlController->getUnifiedGridRenderer();
                if (!liveRenderer) {
                    return;
                }
                if (liveRenderer->getVisibleTimeEnd() <= liveRenderer->getVisibleTimeStart()) {
                    return;
                }
                if (m_candleViewportConn) {
                    disconnect(m_candleViewportConn);
                    m_candleViewportConn = QMetaObject::Connection();
                }
                if (m_connected) {
                    requestCandleHistoryForSymbol(symbol);
                }
            });
        }
        return;
    }
    if (chartDebugEnabled()) {
        sLog_Debug(QString("Candle history request: symbol=%1 tfMs=%2 tfSec=%3 limit=%4 endSec=%5 view=[%6..%7]")
                   .arg(symbol)
                   .arg(rendererTfMs)
                   .arg(timeframeSec)
                   .arg(limit)
                   .arg(endTimeSec)
                   .arg(viewStart)
                   .arg(viewEnd));
    }
    m_dataSource->requestCandleHistory(symbol, timeframeSec, endTimeSec, limit);
}

void MainWindowGPU::closeEvent(QCloseEvent* event) {
    m_layoutOrchestrator->saveLayout("_last_session");
    QMainWindow::closeEvent(event);
}

void MainWindowGPU::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (m_firstShow) {
        m_firstShow = false;

        if (const auto screen = QApplication::primaryScreen()) {
            setGeometry(screen->availableGeometry());
        }

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
    docks.watchlistDock = m_watchlistDock;
    docks.screenerDock = m_screenerDock;
    docks.stockChartDock = m_stockChartDock;
    docks.orderBookDock = m_orderBookDock;

    MenuBuilder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    callbacks.openSecFilingViewer = [this]() { onOpenSecFilingViewer(); };
    callbacks.openFontSettings = [this]() { onOpenFontSettings(); };
    m_menuBuilder->setChartDock(m_heatmapDock);

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


void MainWindowGPU::connectMarketDataSignals() {
    if (!m_qmlController) {
        sLog_Error("Cannot connect signals: QML controller not initialized");
        return;
    }
    auto unifiedGridRenderer = m_qmlController->getUnifiedGridRenderer();
    if (!m_dataSource || !unifiedGridRenderer) {
        sLog_Error("Cannot connect signals: Missing components");
        return;
    }

    if (m_heatmapDock && m_heatmapDock->toolbar()) {
        const auto& serverConfig = GuiConfigStore::instance().serverConfig();
        int64_t tf = serverConfig.heatmap.activeTimeframeMs;
        if (tf <= 0 && !serverConfig.heatmap.timeframesMs.empty()) {
            tf = serverConfig.heatmap.timeframesMs.front();
        }
        if (tf > 0) {
            unifiedGridRenderer->setTimeframe(static_cast<int>(tf));
        }
        m_heatmapDock->toolbar()->setTimeframeMs(unifiedGridRenderer->getCurrentTimeframe());
        if (chartDebugEnabled()) {
            sLog_Debug(QString("Chart TF init: server=%1 renderer=%2")
                       .arg(tf)
                       .arg(unifiedGridRenderer->getCurrentTimeframe()));
        }
        auto* toolbar = m_heatmapDock->toolbar();
        toolbar->setLayerToggleStates(unifiedGridRenderer->heatmapLayerEnabled(),
                                      unifiedGridRenderer->footprintLayerEnabled(),
                                      unifiedGridRenderer->tpoLayerEnabled(),
                                      unifiedGridRenderer->volumeProfileLayerEnabled());
        connect(unifiedGridRenderer,
                &UnifiedGridRenderer::layerVisibilityChanged,
                this,
                [toolbar, unifiedGridRenderer]() {
                    toolbar->setLayerToggleStates(unifiedGridRenderer->heatmapLayerEnabled(),
                                                  unifiedGridRenderer->footprintLayerEnabled(),
                                                  unifiedGridRenderer->tpoLayerEnabled(),
                                                  unifiedGridRenderer->volumeProfileLayerEnabled());
                },
                Qt::QueuedConnection);
    }

    if (m_modeController) {
        connect(m_modeController,
                &ChartModeController::primaryFieldChanged,
                unifiedGridRenderer,
                &UnifiedGridRenderer::setPrimaryField,
                Qt::QueuedConnection);
        unifiedGridRenderer->setPrimaryField(m_modeController->primaryField());
    }

    connect(unifiedGridRenderer,
            &UnifiedGridRenderer::tpoConfigChanged,
            this,
            [this]() {
                if (!m_connected || !m_userSubscribed) {
                    return;
                }
                if (m_currentSymbol.isEmpty()) {
                    return;
                }
                requestTpoHistoryForSymbol(m_currentSymbol);
            },
            Qt::QueuedConnection);

    unifiedGridRenderer->applyClientConfig(GuiConfigStore::instance().clientConfig());
    if (GuiConfigStore::instance().hasServerConfig()) {
        unifiedGridRenderer->applyServerConfig(GuiConfigStore::instance().serverConfig());
    }
    
    auto dataProcessor = unifiedGridRenderer->getDataProcessor();
    if (dataProcessor) {
        connect(m_dataSource.get(), &IGridDataSource::heatmapSliceReceived,
                dataProcessor, &DataProcessor::onHeatmapSliceReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::footprintSliceReceived,
                dataProcessor, &DataProcessor::onFootprintSliceReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::tpoSliceReceived,
                dataProcessor, &DataProcessor::onTpoSliceReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::volumeProfileSliceReceived,
                dataProcessor, &DataProcessor::onVolumeProfileSliceReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::heatmapHistoryReceived,
                dataProcessor, &DataProcessor::onHeatmapHistoryReceived, Qt::QueuedConnection);
    }
    
    // Trade connection (simplified, assume UnifiedGridRenderer has a slot)
    connect(m_dataSource.get(), &IGridDataSource::tradeReceived,
            unifiedGridRenderer, &UnifiedGridRenderer::onTradeReceived, Qt::QueuedConnection);
    
    connect(m_dataSource.get(), &IGridDataSource::connectionStatusChanged,
            this, &MainWindowGPU::onConnectionStatusChanged);
    connect(m_dataSource.get(), &IGridDataSource::connectionStatusChanged,
            this, [this](bool connected) {
                if (!connected || !m_dataSource) {
                    return;
                }
                if (!m_userSubscribed) {
                    return;
                }
                const QString symbol = m_currentSymbol;
                if (symbol.isEmpty()) {
                    return;
                }
                m_dataSource->subscribe(symbol);
                requestHeatmapHistoryForSymbol(symbol);
                requestFootprintHistoryForSymbol(symbol);
                requestTpoHistoryForSymbol(symbol);
                requestCandleHistoryForSymbol(symbol);
            });

    connect(m_dataSource.get(), &IGridDataSource::errorOccurred,
            this, [](const QString& error) {
                sLog_Error("DataSource error: " << error);
            });
}

void MainWindowGPU::onConnectionStatusChanged(bool connected) {
    if (m_statusBar) {
        m_statusBar->setConnectionStatus(connected);
    }
    m_connected = connected;

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

void MainWindowGPU::resetLayoutToDefault() {
    m_layoutOrchestrator->resetLayoutToDefault(getDockWidgets());
}


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

void MainWindowGPU::onOpenFontSettings() {
    if (!m_fontDialog) {
        m_fontDialog = new FontSettingsDialog(this);
    }
    m_fontDialog->show();
    m_fontDialog->raise();
    m_fontDialog->activateWindow();
}


LayoutOrchestrator::DockWidgets MainWindowGPU::getDockWidgets() const {
    LayoutOrchestrator::DockWidgets docks;
    docks.heatmapDock = m_heatmapDock;
    docks.secDock = m_secDock;
    docks.copenetDock = m_copenetDock;
    docks.aiCommentaryDock = m_aiCommentaryDock;
    docks.labDock = m_labDock;
    docks.watchlistDock = m_watchlistDock;
    docks.screenerDock = m_screenerDock;
    docks.stockChartDock = m_stockChartDock;
    docks.orderBookDock = m_orderBookDock;
    docks.paperTradingDock = m_paperTradingDock;
    return docks;
}
