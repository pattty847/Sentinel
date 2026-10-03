#include <QQuickView>
#include <QTabWidget>
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
#include "../core/servermodel/SessionManager.hpp"
#include "render/TpoProfileModel.hpp"
#include "UnifiedGridRenderer.h"
#include "render/DataProcessor.hpp"
#include "render/GridViewState.hpp"
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
#include "widgets/HeatmapTelemetryDock.hpp"
#include "mainwindow/HeatmapChartControls.hpp"
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
#include "config/AgentHostMode.hpp"
#include "mainwindow/QmlSceneController.h"
#include "mainwindow/LayoutOrchestrator.h"
#include "mainwindow/DockVisibilityController.hpp"
#include "mainwindow/MenuBuilder.h"
#include "mainwindow/ShortcutBinder.h"
#include "mainwindow/GuiApiServer.h"
#include "mainwindow/AgentApiCodec.hpp"
#include "render/heatmap/HeatmapDataService.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "protocol/SentinelStreamClientTransport.hpp"
#include "mainwindow/AgentApiSnapshots.hpp"
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
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
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
}

MainWindowGPU::MainWindowGPU(QWidget* parent) : QMainWindow(parent) {
    m_agentApiSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto& clientConfig = GuiConfigStore::instance().clientConfig();
    auto remote = std::make_unique<RemoteGridDataSource>(
        QString::fromStdString(clientConfig.server.host),
        QString::fromStdString(clientConfig.server.port),
        QString::fromStdString(clientConfig.server.caFile));
    m_heatmapSettings = std::make_unique<heatmap::HeatmapSettingsModel>(m_heatmapSettingsStore, "main",
                                                                         clientConfig.heatmap);
    if (const auto& renderer = GuiConfigStore::instance().heatmapRendererOverride(); !renderer.isEmpty()) {
        // --heatmap-renderer: this process only; later persisted patches and
        // workspace snapshots never carry it (they save from the stored settings).
        m_heatmapSettings->setProcessRenderer(renderer.toStdString());
        sLog_App("Heatmap renderer override (process only): " << renderer);
    }
    m_heatmapControls = new HeatmapChartControls(m_heatmapSettings.get(), this);
    // Attach BEFORE connect: the adapter learns connection state only from the
    // signal, and subscribe immediately pushes availability. No chart/controller
    // is created in S6a, in either configured renderer mode.
    m_heatmapDataService = std::make_unique<heatmap::HeatmapDataService>(
        [client = remote->streamClient()](QObject *) {
            return new protocol::SentinelStreamClientTransport(*client);
        }, m_heatmapSettings->budgets());
    // Budgets tab: the process tiers apply to the live service at once.
    m_heatmapSettings->setBudgetSink([this](const heatmap::HeatmapBudgets &budgets) {
        return m_heatmapDataService && m_heatmapDataService->setBudgets(budgets);
    });
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
        m_serverConfigReady = true;
        if (m_qmlController) {
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->applyServerConfig(config);
                if (m_heatmapDock && m_heatmapDock->toolbar()) {
                    m_heatmapDock->toolbar()->setAvailableTimeframes(config.heatmap.servedTimeframesMs);
                    m_heatmapDock->toolbar()->setTimeframeMs(renderer->getCurrentTimeframe());
                }
            }
        }
        if (!config.defaultSymbols.empty() && !m_userSubscribed && !m_symbolSelectionRequested
            && m_refusedSymbol.isEmpty()
            && AgentHostMode::symbolAllowed(QString::fromStdString(config.defaultSymbols.front()).trimmed().toUpper())) {
            const QString defaultSymbol = QString::fromStdString(config.defaultSymbols.front()).trimmed().toUpper();
            sLog_App("Default symbol from server config: symbol=" << defaultSymbol
                     << " prev=" << m_currentSymbol);
            if (m_symbolInput) {
                m_symbolInput->setText(defaultSymbol);
            }
            if (m_currentSymbol != defaultSymbol) {
                if (m_connected) {
                    m_initialSubscriptionAttempted = true;
                    applySubscriptionActions(m_symbolSubscriptions.request("main", defaultSymbol));
                }
                else m_currentSymbol = defaultSymbol;
            }
        }
        if (m_connected && m_userSubscribed) {
            requestConfiguredHistoryForSymbol(m_currentSymbol);
        }
    });

    // Track render-thread frame work and chart interaction for the status readout.
    if (m_qquickView) {
        PerformanceMonitor::instance().attachToWindow(m_qquickView);
    }
    if (m_statusBar) {
        auto& perfMon = PerformanceMonitor::instance();
        connect(&perfMon, &PerformanceMonitor::frameStatsChanged, m_statusBar, &StatusBar::setFrameStats);
        connect(&perfMon, &PerformanceMonitor::cpuUsageChanged, m_statusBar, &StatusBar::setCpuUsage);
        connect(&perfMon, &PerformanceMonitor::gpuUsageChanged, m_statusBar, &StatusBar::setGpuUsage);
        connect(&perfMon, &PerformanceMonitor::latencyChanged, m_statusBar, &StatusBar::setLatency);
        connect(&perfMon, &PerformanceMonitor::uploadBandwidthChanged, m_statusBar, &StatusBar::setUploadBandwidth);
        if (auto* remote = dynamic_cast<RemoteGridDataSource*>(m_dataSource.get())) {
            connect(remote->streamClient(), &SentinelStreamClient::coinbaseLatencyReceived,
                    m_statusBar, &StatusBar::setCoinbaseLatency, Qt::QueuedConnection);
        }
    }
    
    m_modeController = new ChartModeController(this);
    if (m_qmlController) {
        m_qmlController->setChartModeController(m_modeController);
        // --agent-host: start on an allowlisted symbol, or on none (stay unsubscribed).
        const QString defaultSymbol = AgentHostMode::startupSymbol(QStringLiteral("BTC-USD"));
        m_qmlController->updateSymbolInContext(defaultSymbol);  // Default symbol
        m_currentSymbol = defaultSymbol;
    }
    m_modeController->setPrimaryField(ChartModeController::PrimaryField::Heatmap);
    m_modeController->setCandlesEnabled(true);
    m_layoutOrchestrator = std::make_unique<LayoutOrchestrator>(this);
    m_dockVisibility = std::make_unique<DockVisibilityController>(this);
    for (const auto& [id, dock] : LayoutOrchestrator::apiDocks(getDockWidgets()))
        m_dockVisibility->add(id, dock);
    m_layoutOrchestrator->setHeatmapHooks(
        // Named workspaces only; the model ignores _last_session (INV-088). A
        // restore emits changed(): the chart, toolbar, dialog and dock follow.
        [this](const QString &name) { m_heatmapSettings->saveLayout(name); },
        [this](const QString &name) {
            m_heatmapSettings->restoreLayout(name);
            // The dock's saved Qt state may disagree with showTelemetry: the setting wins.
            m_heatmapControls->requestTelemetryVisible(m_heatmapSettings->settings().showTelemetry);
        });
    // Defer arrangeDefaultLayout() until after show: resizeDocks() fails at default 640x480.
    m_menuBuilder = std::make_unique<MenuBuilder>(menuBar());
    m_shortcutBinder = std::make_unique<ShortcutBinder>(this);
    setupMenuBar();
    setupShortcuts();
    
    setupConnections();
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            connect(renderer, &UnifiedGridRenderer::timeframeChanged, this, [this]() {
                ++m_agentApiSelectionEpoch;
                m_heatmapReceivedAtMs.reset();
                m_candlesReceivedAtMs.reset();
                requestCandleHistoryForSymbol(m_currentSymbol);
            });
            connect(renderer, &UnifiedGridRenderer::viewportChanged, this, [this]() {
                requestCandleHistoryForSymbol(m_currentSymbol);
            });
        }
    }
    setWindowProperties();
    setupGuiApiServer();
    // Register serverConfigUpdated and the other GUI consumers before starting
    // the client too. A hello/setTimeframe may still precede chunk availability;
    // it must not synchronously construct a GPU layer from availability (S6b).
    static_cast<RemoteGridDataSource *>(m_dataSource.get())->connectToServer();
    
    if (!validateComponents()) {
        sLog_Error("Component validation failed, app may not function: qmlController="
                   << (m_qmlController != nullptr)
                   << " qmlValid=" << (m_qmlController && m_qmlController->isValid())
                   << " renderer=" << (m_qmlController && m_qmlController->getUnifiedGridRenderer())
                   << " dataSource=" << (m_dataSource != nullptr));
        QMessageBox::critical(this, "Initialization Error", "Failed to initialize core components. Check logs.");
    }
}

MainWindowGPU::~MainWindowGPU() {
    // Members (the heatmap data service) die before the base QWidget deletes the
    // docks and the chart: detach the chart's GPU layer while both are alive.
    if (auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr)
        renderer->setHeatmapService(nullptr);
    // The controls point at the settings model (a member): end them first.
    delete m_heatmapControls;
    m_heatmapControls = nullptr;
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
    m_heatmapTelemetryDock = new HeatmapTelemetryDock(this);
    // In a dock area from the start (a _last_session restore that predates the dock
    // leaves it there); shown by the showTelemetry setting (HeatmapChartControls).
    addDockWidget(Qt::RightDockWidgetArea, m_heatmapTelemetryDock);
    m_heatmapTelemetryDock->hide();
    m_heatmapControls->setTelemetryDock(m_heatmapTelemetryDock);
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
            if (m_modeController) {
                m_modeController->setPrimaryField(field);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::heatmapToggled, this, [this](bool enabled) {
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setHeatmapLayerEnabled(enabled);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::footprintToggled, this, [this](bool enabled) {
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setFootprintLayerEnabled(enabled);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::tpoToggled, this, [this](bool enabled) {
            if (!m_qmlController) return;
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                renderer->setTpoLayerEnabled(enabled);
            }
            if (enabled && m_connected && m_userSubscribed) {
                requestTpoHistoryForSymbol(m_currentSymbol);
            }
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::volumeProfileToggled, this, [this](bool enabled) {
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
            if (!renderer) return;
            // value is raw 0–1000 slider position.
            // Convert to log-scaled threshold using the observed data range.
            // Dead zone: bottom 4% of slider (value < 40) = threshold 0 (off).
            constexpr double kSliderMax = 1000.0;
            constexpr double kDeadZone  = 0.04;
            const double ratio = value / kSliderMax;
            double threshold = 0.0;
            if (ratio >= kDeadZone) {
                const double obsMin = renderer->heatmapMinObservedLiquidity();
                const double obsMax = renderer->heatmapMaxObservedLiquidity();
                const double minVal = (obsMin > 0.0 && obsMin < obsMax) ? obsMin : (obsMax > 0.0 ? obsMax / 1000.0 : 1e-9);
                const double maxVal = obsMax > 0.0 ? obsMax : 1e-6;
                const double r = (ratio - kDeadZone) / (1.0 - kDeadZone);
                const double logSpan = std::log(maxVal / minVal);
                threshold = minVal * std::exp(r * logSpan);
            }
            renderer->setProperty("heatmapLiquidityThreshold", threshold);
        });
        // Label currency, labels on/off and the GPU range slider go through the
        // chart settings model (HeatmapChartControls::setToolbar).
        connect(m_heatmapDock->toolbar(), &TopToolbar::colorPresetSelected, this, [this](const QString& preset) {
            // The chart palette is a persisted chart setting both renderers draw (S6b).
            if (const auto error = m_heatmapSettings->apply({{"palettePreset", preset}}); !error.isEmpty())
                sLog_Warning("Palette preset rejected: " << preset << " " << error);
        });
        // Candle style: HeatmapChartControls (the toolbar combo and the chart menu).
        connect(m_heatmapDock->toolbar(), &TopToolbar::subscribeRequested, this, &MainWindowGPU::onSubscribe);
        // The tick selector (S6c) reads and writes the chart settings model.
        m_heatmapControls->setToolbar(m_heatmapDock->toolbar());
        connect(m_heatmapDock->toolbar(), &TopToolbar::settingsRequested, this, [this]() {
            if (auto* dialog = openHeatmapSettingsDialog()) dialog->activateWindow();
        });
        connect(m_heatmapDock->toolbar(), &TopToolbar::screenshotRequested, this, [this]() { saveChartScreenshot(); });
        // The chart settings menu (gear): the actions this window owns.
        HeatmapChartControls::MenuHooks hooks;
        hooks.openSettings = [this](const QString& tab) {
            if (auto* dialog = openHeatmapSettingsDialog()) {
                for (int i = 0; i < dialog->tabs()->count(); ++i)
                    if (dialog->tabs()->tabText(i) == tab) dialog->tabs()->setCurrentIndex(i);
                dialog->activateWindow();
            }
        };
        hooks.screenshot = [this]() { saveChartScreenshot(); };
        hooks.saveLayout = [this]() { onSaveLayout(); };
        hooks.restoreLayout = [this]() { onRestoreLayout(); };
        hooks.resetLayout = [this]() { onResetLayout(); };
        hooks.fontSettings = [this]() { onOpenFontSettings(); };
        m_heatmapControls->setMenuHooks(std::move(hooks));
        connect(m_heatmapDock->toolbar(), &TopToolbar::timeframeSelected, this, [this](const QString& label) {
            const int ms = timeframeMsFromLabel(label);
            if (ms <= 0) {
                sLog_Warning("Unknown timeframe label ignored: label=" << label);
                return;
            }
            selectTimeframe(ms);
        });
    }
    
    setUpdatesEnabled(true);
}

// The toolbar camera and the chart menu: the chart's own scene-graph grab (never
// screen pixels) into the configured screenshot directory.
QString MainWindowGPU::saveChartScreenshot() {
    QQuickView* view = m_heatmapDock ? m_heatmapDock->qquickView() : nullptr;
    if (!view || !view->isVisible()) {
        sLog_Warning("Chart screenshot skipped: the chart is not visible");
        return {};
    }
    const auto& clientConfig = GuiConfigStore::instance().clientConfig();
    QString dirPath = qEnvironmentVariable("SENTINEL_GUI_SCREENSHOT_DIR");
    if (dirPath.isEmpty()) dirPath = QString::fromStdString(clientConfig.gui.screenshotDir);
    if (dirPath.isEmpty()) dirPath = QDir::currentPath() + "/screenshots";
    QDir dir(dirPath);
    const QImage image = view->grabWindow();
    const QString path = dir.filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz") + "_chart_" +
                                      m_currentSymbol + ".png");
    if (image.isNull() || !dir.mkpath(".") || !image.save(path, "PNG")) {
        sLog_Warning("Chart screenshot failed: path=" << path);
        return {};
    }
    sLog_App("Saved chart screenshot to " << path);
    statusBar()->showMessage("Screenshot saved: " + path, 5000);
    return path;
}

HeatmapSettingsDialog* MainWindowGPU::openHeatmapSettingsDialog() {
    auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
    if (!renderer) return nullptr;
    if (!m_heatmapSettingsDialog) {
        m_heatmapSettingsDialog = new HeatmapSettingsDialog(m_heatmapSettings.get(), renderer, this);
        m_heatmapSettingsDialog->setTpoDefaults(GuiConfigStore::instance().clientConfig().tpo);
        m_heatmapControls->setDialog(m_heatmapSettingsDialog);
    } else {
        m_heatmapSettingsDialog->setRenderer(renderer);
        m_heatmapSettingsDialog->refreshFromModel();
    }
    m_heatmapSettingsDialog->show();
    m_heatmapSettingsDialog->raise();
    return m_heatmapSettingsDialog;
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
            sLog_Warning("Invalid SENTINEL_GUI_API_PORT, using config port: value="
                         << qEnvironmentVariable("SENTINEL_GUI_API_PORT") << " port=" << defaultPort);
        }
    }

    if (port == 0) {
        sLog_App("GUI API disabled (api_port=0)");
        return;
    }
    if (port < 0 || port > 65535) {
        sLog_Warning("GUI API port out of range, using config port: port=" << port
                     << " configPort=" << defaultPort);
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
    // --agent-host: the host owns this directory; config and the env var must not redirect it.
    if (AgentHostMode::active()) screenshotDir = AgentHostMode::screenshotDir();

    m_guiApiServer = std::make_unique<GuiApiServer>(this,
                                                    m_heatmapDock ? m_heatmapDock->qquickView() : nullptr,
                                                    m_labDock ? m_labDock->qquickView() : nullptr,
                                                    [this]() { return agentApiStateSnapshot(); },
                                                    [this]() { return agentApiViewportSnapshot(); },
                                                    [this](const AgentApi::ValidationResult& q) { return agentApiCandlesSnapshot(q); },
                                                    [this](int levels) { return agentApiBookSnapshot(levels); },
                                                    [this](qint64 windowMs, int limit) { return agentApiTradesSnapshot(windowMs, limit); },
                                                    [this](const heatmap_window::WallQuery& query,
                                                           std::function<void(heatmap_window::WallsSnapshot)> complete) {
                                                        auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
                                                        auto* processor = renderer ? renderer->getDataProcessor() : nullptr;
                                                        if (renderer && renderer->gpuHeatmapActive()) {
                                                            renderer->gpuHeatmapLayer()->scanWalls(query, std::move(complete));
                                                            return;
                                                        }
                                                        if (!processor) {
                                                            heatmap_window::WallsSnapshot unavailable;
                                                            unavailable.status = 503;
                                                            complete(std::move(unavailable));
                                                            return;
                                                        }
                                                        QMetaObject::invokeMethod(processor,
                                                            [processor, renderer, query, complete = std::move(complete)]() mutable {
                                                                auto snapshot = processor->captureHeatmapWalls(query);
                                                                QMetaObject::invokeMethod(renderer,
                                                                    [snapshot = std::move(snapshot), complete = std::move(complete)]() mutable {
                                                                        complete(std::move(snapshot));
                                                                    }, Qt::QueuedConnection);
                                                            }, Qt::QueuedConnection);
                                                    },
                                                    [this](const QString& kind, const AgentApi::ControlBody& body) { return agentApiApplyControl(kind, body); },
                                                    [this]() {
                                                        auto* r = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
                                                        return std::pair<quint64, quint64>{r ? r->renderedControlRevision() : 0,
                                                                                          r ? r->renderedFrameId() : 0};
                                                    },
                                                    [this](quint64 revision) {
                                                        if (auto* r = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr)
                                                            r->setAgentControlRevision(revision, m_agentApiSelectionEpoch,
                                                                                       agentApiViewportSnapshot().viewportVersion.value_or(0));
                                                    },
                                                    this);
    m_guiApiServer->setHeatmapSnapshot([this] { return agentApiHeatmapSnapshot(); });
    m_guiApiServer->setDocksSnapshot([this] { return m_dockVisibility->snapshot(); });
    // S6c widget targets: the settings dialog (opened on demand, a given tab) and
    // the telemetry dock, grabbed from their own painting (never screen pixels).
    m_guiApiServer->setWidgetGrab([this](const QString& target, QString* error) -> QImage {
        if (target == "telemetry") {
            if (!m_heatmapTelemetryDock || !m_heatmapTelemetryDock->exposed()) {
                if (error) *error = "telemetry_not_visible";
                return {};
            }
            m_heatmapTelemetryDock->refresh();
            return m_heatmapTelemetryDock->grab().toImage();
        }
        if (target == "chartmenu") {
            auto* toolbar = m_heatmapDock ? m_heatmapDock->toolbar() : nullptr;
            if (!toolbar || !toolbar->chartMenu()) {
                if (error) *error = "toolbar_not_visible";
                return {};
            }
            m_heatmapControls->refreshChartMenu();
            toolbar->chartMenu()->ensurePolished();
            toolbar->chartMenu()->adjustSize();
            return toolbar->chartMenu()->grab().toImage();
        }
        if (target == "toolbar") {
            auto* toolbar = m_heatmapDock ? m_heatmapDock->toolbar() : nullptr;
            if (!toolbar || !toolbar->isVisible()) {
                if (error) *error = "toolbar_not_visible";
                return {};
            }
            return toolbar->grab().toImage();
        }
        auto* dialog = openHeatmapSettingsDialog();
        if (!dialog) {
            if (error) *error = "settings_unavailable";
            return {};
        }
        if (target.startsWith("settings:"))
            for (int i = 0; i < dialog->tabs()->count(); ++i)
                if (dialog->tabs()->tabText(i) == target.mid(9)) dialog->tabs()->setCurrentIndex(i);
        return dialog->grab().toImage();
    });
    if (!m_guiApiServer->start(static_cast<quint16>(port), screenshotDir)) {
        sLog_Error("GUI API failed to bind on port " << port << ": " << m_guiApiServer->errorString());
    }
}

void MainWindowGPU::startScreenerServer() {
    if (m_screenerProcess && m_screenerProcess->state() != QProcess::NotRunning) {
        return;  // already running
    }
    if (!GuiConfigStore::instance().clientConfig().gui.startScreener) {
        sLog_App("Screener server not started: --no-screener");
        return;
    }

    // Locate scripts/screener/screener_server.py relative to the running binary.
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    for (const QString& rel : {
             QStringLiteral("../../../../scripts"),         // dev: build/mac-clang/apps/sentinel-gui/ -> repo/scripts
             QStringLiteral("../../../scripts"),
             QStringLiteral("../../scripts"),
             QStringLiteral("../scripts"),
             QStringLiteral("scripts"),
         }) {
        candidates << QDir(appDir).absoluteFilePath(rel);
    }

    QString scriptsDir;
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate + QStringLiteral("/screener/screener_server.py")).exists()) {
            scriptsDir = candidate;
            break;
        }
    }

    if (scriptsDir.isEmpty()) {
        sLog_Warning("Screener server not started: scripts/screener/screener_server.py not found near appDir="
                     << appDir);
        return;
    }

    // Kill any stale process holding port 17200 from a previous run.
    QProcess::execute(QStringLiteral("sh"),
        {QStringLiteral("-c"),
         QStringLiteral("lsof -ti tcp:17200 | xargs kill -9 2>/dev/null; true")});

    // Use 'uv run' so Python deps are automatically resolved from the venv.
    const QString uvBin = QStringLiteral("uv");
    QStringList args;
    args << QStringLiteral("run")
         << QStringLiteral("python")
         << QStringLiteral("screener/screener_server.py");

    if (!m_screenerProcess) {
        m_screenerProcess = new QProcess(this);
        // Capture both stdout and stderr so we see Python tracebacks.
        auto logOutput = [this]() {
            const QString out = QString::fromUtf8(m_screenerProcess->readAllStandardOutput()).trimmed();
            if (!out.isEmpty()) sLog_App("[screener_server] " << out);
        };
        auto logErr = [this]() {
            const QString err = QString::fromUtf8(m_screenerProcess->readAllStandardError()).trimmed();
            if (!err.isEmpty()) sLog_App("[screener_server] " << err);
        };
        connect(m_screenerProcess, &QProcess::readyReadStandardOutput, this, logOutput);
        connect(m_screenerProcess, &QProcess::readyReadStandardError,  this, logErr);
        connect(m_screenerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, scriptsDir, uvBin, args](int exitCode, QProcess::ExitStatus status) {
                    if (!m_screenerProcess) {  // deliberate shutdown
                        sLog_App("Screener server stopped: code=" << exitCode
                                 << " status=" << static_cast<int>(status));
                        return;
                    }
                    sLog_Warning("Screener server exited: code=" << exitCode
                                 << " status=" << static_cast<int>(status));
                    ++m_screenerRestartCount;
                    if (m_screenerRestartCount > kMaxScreenerRestarts) {
                        sLog_Warning("Screener server failed " << m_screenerRestartCount
                                     << " times in a row, giving up. Kill port 17200 and restart the app.");
                        return;
                    }
                    const int delayMs = 2000 * m_screenerRestartCount;  // back off: 2s, 4s, 6s
                    sLog_App("Screener server restarting (attempt " << m_screenerRestartCount
                             << "/" << kMaxScreenerRestarts << ") in " << delayMs << "ms...");
                    QTimer::singleShot(delayMs, this, [this, scriptsDir, uvBin, args]() {
                        if (!m_screenerProcess) return;
                        // Kill stale process before retrying too.
                        QProcess::execute(QStringLiteral("sh"),
                            {QStringLiteral("-c"),
                             QStringLiteral("lsof -ti tcp:17200 | xargs kill -9 2>/dev/null; true")});
                        m_screenerProcess->setWorkingDirectory(scriptsDir);
                        m_screenerProcess->start(uvBin, args);
                    });
                });
    }

    m_screenerRestartCount = 0;
    m_screenerProcess->setWorkingDirectory(scriptsDir);
    m_screenerProcess->start(uvBin, args);
    if (m_screenerProcess->waitForStarted(2000)) {
        sLog_App("Screener server started: pid=" << m_screenerProcess->processId()
                 << " dir=" << scriptsDir);
    } else {
        sLog_Warning("Screener server failed to start: program=" << uvBin
                     << " dir=" << scriptsDir << " error=" << m_screenerProcess->errorString());
    }
}

void MainWindowGPU::stopScreenerServer() {
    if (!m_screenerProcess || m_screenerProcess->state() == QProcess::NotRunning) {
        m_screenerProcess = nullptr;
        return;
    }
    // Null first so the finished() handler doesn't schedule an auto-restart.
    QProcess* proc = m_screenerProcess;
    m_screenerProcess = nullptr;
    proc->terminate();
    if (!proc->waitForFinished(2000)) {
        proc->kill();
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
    if (!subscribeSymbol(symbol)) {
        sLog_App("ui: subscribe rejected, invalid symbol=" << symbol);
        QMessageBox::warning(this, "Invalid Input", "Enter a valid symbol like BTC-USD.");
    }
}

bool MainWindowGPU::subscribeSymbol(const QString& symbol) {
    static const QRegularExpression pattern("^[A-Z0-9]{2,20}-[A-Z0-9]{2,20}$");
    if (!pattern.match(symbol).hasMatch()) return false;
    if (!AgentHostMode::symbolAllowed(symbol)) { // --agent-host: no upstream subscriptions beyond the allowlist
        sLog_Warning("agent-host: symbol refused: symbol=" << symbol);
        return false;
    }
    sLog_App("ui: subscribe symbol=" << symbol << " prev=" << m_currentSymbol
             << " connected=" << m_connected);
    m_symbolSelectionRequested = true;
    m_initialSubscriptionAttempted = true;
    m_refusedSymbol.clear();
    statusBar()->clearMessage();
    if (m_connected) applySubscriptionActions(m_symbolSubscriptions.request("main", symbol));
    else m_offlineRequestedSymbol = symbol;
    if (m_symbolInput) m_symbolInput->setText(symbol);
    return true;
}

void MainWindowGPU::applySubscriptionActions(const QVector<SymbolSubscriptionManager::Action>& actions) {
    for (const auto& action : actions) {
        switch (action.kind) {
        case SymbolSubscriptionManager::Action::Subscribe:
            if (m_dataSource) m_dataSource->subscribe(action.symbol);
            break;
        case SymbolSubscriptionManager::Action::Unsubscribe:
            if (m_dataSource) m_dataSource->unsubscribe(action.symbol);
            break;
        case SymbolSubscriptionManager::Action::Activate:
            if (action.consumer != QLatin1String("main")) break;
            m_userSubscribed = true;
            m_refusedSymbol.clear();
            if (m_qmlController) m_qmlController->updateSymbolInContext(action.symbol);
            propagateSymbolChange(action.symbol);
            if (m_symbolInput) m_symbolInput->setText(action.symbol);
            if (m_connected) {
                requestConfiguredHistoryForSymbol(action.symbol);
                requestTpoHistoryForSymbol(action.symbol);
            }
            break;
        case SymbolSubscriptionManager::Action::Refused:
            if (action.consumer == QLatin1String("main")) {
                m_refusedSymbol = action.symbol;
                if (m_symbolInput) m_symbolInput->setText(m_currentSymbol);
            }
            break;
        }
    }
}

void MainWindowGPU::selectTimeframe(int ms) {
    auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
    if (!renderer) return;
    sLog_App("ui: timeframe selected tfMs=" << ms << " symbol=" << m_currentSymbol);
    renderer->setTimeframe(ms);
    if (m_heatmapDock && m_heatmapDock->toolbar()) m_heatmapDock->toolbar()->setTimeframeMs(ms);
    if (m_connected && m_userSubscribed) {
        requestHeatmapHistoryForSymbol(m_currentSymbol);
        requestCandleHistoryForSymbol(m_currentSymbol);
        if (m_modeController && m_modeController->primaryField() == 1) requestFootprintHistoryForSymbol(m_currentSymbol);
        else if (m_modeController && (m_modeController->primaryField() == 2 || m_modeController->primaryField() == 3))
            requestTpoHistoryForSymbol(m_currentSymbol);
    }
}

void MainWindowGPU::propagateSymbolChange(const QString& symbol) {
    if (m_currentSymbol != symbol) {
        ++m_agentApiSelectionEpoch;
        m_heatmapReceivedAtMs.reset();
        m_candlesReceivedAtMs.reset();
        m_bookReceivedAtMs.reset();
        m_tradesReceivedAtMs.reset();
    }
    m_currentSymbol = symbol;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            renderer->setActiveSymbol(symbol);
        }
    }
    requestCandleHistoryForSymbol(symbol);
    emit symbolChanged(symbol);
}

bool MainWindowGPU::canRequestConfiguredHistoryForSymbol(const QString& symbol) const {
    return m_connected && m_userSubscribed && m_serverConfigReady && m_dataSource &&
           !symbol.isEmpty() && symbol == m_currentSymbol;
}

void MainWindowGPU::requestConfiguredHistoryForSymbol(const QString& symbol) {
    if (!canRequestConfiguredHistoryForSymbol(symbol)) {
        return;
    }
    requestHeatmapHistoryForSymbol(symbol);
    requestFootprintHistoryForSymbol(symbol);
    requestCandleHistoryForSymbol(symbol);
}

void MainWindowGPU::requestHeatmapHistoryForSymbol(const QString& symbol) {
    if (!canRequestConfiguredHistoryForSymbol(symbol)) {
        return;
    }
    if (auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
        renderer && renderer->gpuHeatmapActive()) {
        return; // the GPU layer's controller plans and fetches its own chunks
    }
    const auto& store = GuiConfigStore::instance();
    if (store.clientConfig().heatmap.source == "recording" &&
        store.serverConfig().wasAdvertised("recording.available") && store.serverConfig().recording.available) {
        if (m_qmlController) {
            if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
                QMetaObject::invokeMethod(renderer->getDataProcessor(), &DataProcessor::refreshRecordingHistory,
                                          Qt::QueuedConnection);
            }
        }
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
    sLog_Data("Heatmap history request: symbol=" << symbol << " tfMs=" << tf
              << " count=" << count);
    m_dataSource->requestHeatmapHistory(symbol, tf, 0, count);
}

void MainWindowGPU::requestFootprintHistoryForSymbol(const QString& symbol) {
    if (!canRequestConfiguredHistoryForSymbol(symbol)) {
        return;
    }
    int64_t timeframeMs = 0;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            timeframeMs = renderer->getCurrentTimeframe();
        }
    }
    const auto& overlays = GuiConfigStore::instance().serverConfig().tradeOverlays;
    if (timeframeMs <= 0) timeframeMs = overlays.footprintTimeframeMs;
    const int count = std::clamp(overlays.gridWidth, 1, 256);
    sLog_Data("Footprint history request: symbol=" << symbol << " tfMs=" << timeframeMs
              << " count=" << count);
    m_dataSource->requestFootprintHistory(symbol, timeframeMs, 0, count);
}

void MainWindowGPU::requestTpoHistoryForSymbol(const QString& symbol) {
    if (!m_dataSource || symbol.isEmpty()) {
        return;
    }
    int64_t timeframeMs = 1800000;
    int sessionType = 4;
    int sessions = 5;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            timeframeMs = renderer->tpoTimeframeMs();
            sessionType = renderer->tpoSessionType();
            sessions = renderer->tpoSessions();
        }
    }
    timeframeMs = tpo::resolvePeriodMs(sessionType, timeframeMs);
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            if (auto* processor = renderer->getDataProcessor()) {
                QMetaObject::invokeMethod(processor, [processor, timeframeMs, sessionType] {
                    processor->setTpoSelection(timeframeMs, sessionType);
                }, Qt::QueuedConnection);
            }
        }
    }
    // Long sessions (W1, M1) arrive in pages of <= 7 days; pages are sent one at a
    // time (the server rejects more than eight queued overlay history requests).
    const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto pages = tpo::historyPages(sessionType, timeframeMs, nowMs, sessions,
                                         tpo::historyPageBudget(sessionType, timeframeMs, sessions));
    const tpo::HistoryPager::Selection selection{symbol.toStdString(), timeframeMs, sessionType, sessions};
    const bool running = m_tpoPager.busy();
    const auto first = m_tpoPager.start(selection, pages, nowMs);
    sLog_Data("TPO history: symbol=" << symbol << " tfMs=" << timeframeMs
              << " session=" << tpo::sessionTypeName(sessionType) << " sessions=" << sessions
              << " pages=" << pages.size()
              << (running && !first ? " (already paging this selection)" : "")
              << " generation=" << m_tpoPager.generation());
    sendTpoHistoryPage(first);
}

void MainWindowGPU::sendTpoHistoryPage(const std::optional<tpo::HistoryPager::Request>& request) {
    if (m_dataSource) {
        for (const auto& abandoned : m_tpoPager.takeAbandoned()) {
            sLog_Probe("tpo.history", "cancel id=" << abandoned.requestId);
            m_dataSource->cancelTpoHistory(QString::fromStdString(abandoned.symbol),
                                           QString::fromStdString(abandoned.requestId));
        }
    }
    if (!request || !m_dataSource) {
        if (m_tpoPagerTimer && !m_tpoPager.busy()) m_tpoPagerTimer->stop();
        return;
    }
    sLog_Probe("tpo.history", "send id=" << request->requestId << " end=" << request->endMs
               << " count=" << request->count << " pending=" << m_tpoPager.pending());
    m_dataSource->requestTpoHistory(QString::fromStdString(request->symbol), request->periodMs,
                                    request->sessionType, request->endMs, request->count,
                                    QString::fromStdString(request->requestId));
    if (!m_tpoPagerTimer) {
        m_tpoPagerTimer = new QTimer(this);
        m_tpoPagerTimer->setInterval(5000);
        connect(m_tpoPagerTimer, &QTimer::timeout, this, [this] {
            const auto next = m_tpoPager.onTick(QDateTime::currentMSecsSinceEpoch());
            if (next) sLog_Warning("TPO history page timed out; continuing with the next page");
            sendTpoHistoryPage(next);
        });
    }
    if (!m_tpoPagerTimer->isActive()) m_tpoPagerTimer->start();
}

void MainWindowGPU::requestCandleHistoryForSymbol(const QString& symbol) {
    auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
    if (!renderer || !m_dataSource) return;
    const int64_t tfSec = std::max<int64_t>(1, (renderer->getCurrentTimeframe() + 999LL) / 1000);
    const bool ready = canRequestConfiguredHistoryForSymbol(symbol);
    // Selection is updated even before readiness/viewport initialization, so a
    // queued reply from the previous selection can never enter the new series.
    m_dataSource->setCandleHistoryViewport(symbol, tfSec,
        ready ? renderer->getVisibleTimeStart() : 0,
        ready ? renderer->getVisibleTimeEnd() : 0);
}

void MainWindowGPU::closeEvent(QCloseEvent* event) {
    stopScreenerServer();
    m_dockVisibility->restoreBeforeSessionSave();
    m_layoutOrchestrator->saveLayout("_last_session");
    QMainWindow::closeEvent(event);
}

void MainWindowGPU::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (m_firstShow) {
        m_firstShow = false;

        startScreenerServer();

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
            m_dockVisibility->restore();
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
    docks.paperTradingDock = m_paperTradingDock;
    docks.heatmapTelemetryDock = m_heatmapTelemetryDock;

    MenuBuilder::Callbacks callbacks;
    callbacks.saveLayout = [this]() { onSaveLayout(); };
    callbacks.restoreLayout = [this]() { onRestoreLayout(); };
    callbacks.resetLayout = [this]() { onResetLayout(); };
    callbacks.openSecFilingViewer = [this]() { onOpenSecFilingViewer(); };
    callbacks.openFontSettings = [this]() { onOpenFontSettings(); };
    callbacks.toggleSidePanel = [this]() { toggleSidePanel(); };
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
        sLog_Error("Cannot connect signals: missing components dataSource=" << (m_dataSource != nullptr)
                   << " renderer=" << (unifiedGridRenderer != nullptr));
        return;
    }

    const auto& configStore = GuiConfigStore::instance();
    m_serverConfigReady = configStore.hasServerConfig();
    if (m_heatmapDock && m_heatmapDock->toolbar()) {
        const auto& serverConfig = configStore.serverConfig();
        int64_t tf = m_serverConfigReady ? serverConfig.heatmap.activeTimeframeMs : 0;
        if (tf <= 0 && m_serverConfigReady && !serverConfig.heatmap.timeframesMs.empty()) {
            tf = serverConfig.heatmap.timeframesMs.front();
        }
        if (tf > 0) {
            unifiedGridRenderer->setTimeframe(static_cast<int>(tf));
        }
        if (m_serverConfigReady) {
            m_heatmapDock->toolbar()->setAvailableTimeframes(serverConfig.heatmap.servedTimeframesMs);
        }
        m_heatmapDock->toolbar()->setTimeframeMs(unifiedGridRenderer->getCurrentTimeframe());
        sLog_App("Chart timeframe init: serverTfMs=" << tf
                 << " rendererTfMs=" << unifiedGridRenderer->getCurrentTimeframe());
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

    if (m_modeController && m_heatmapDock && m_heatmapDock->toolbar()) {
        // Candles toggled elsewhere (the Agent API layers route) show on the toolbar too.
        connect(m_modeController, &ChartModeController::candlesEnabledChanged, m_heatmapDock->toolbar(),
                &TopToolbar::setCandlesChecked);
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

    // Scroll-past-cache: fetch older heatmap history when the user pans past the edge.
    connect(unifiedGridRenderer,
            &UnifiedGridRenderer::heatmapHistoryNeeded,
            this,
            [this](int64_t timeframeMs, int64_t endTimeMs, int count) {
                if (!canRequestConfiguredHistoryForSymbol(m_currentSymbol)) return;
                m_dataSource->requestHeatmapHistory(m_currentSymbol, timeframeMs, endTimeMs, count);
            },
            Qt::QueuedConnection);

    unifiedGridRenderer->applyClientConfig(GuiConfigStore::instance().clientConfig());
    if (GuiConfigStore::instance().hasServerConfig()) {
        unifiedGridRenderer->applyServerConfig(GuiConfigStore::instance().serverConfig());
    }
    
    auto dataProcessor = unifiedGridRenderer->getDataProcessor();
    unifiedGridRenderer->setActiveSymbol(m_currentSymbol);
    // S6b: the GPU heatmap layer shares the process service (attached before the
    // stream client connected, INV-088); the chart settings pick the renderer.
    unifiedGridRenderer->setHeatmapService(m_heatmapDataService.get());
    // S6c: the controls apply the tick memory, settings and renderer, then keep the
    // chart, toolbar, dialog and telemetry dock on the settings model.
    m_heatmapControls->setRenderer(unifiedGridRenderer);
    if (m_heatmapDock && m_heatmapDock->toolbar()) {
        auto* toolbar = m_heatmapDock->toolbar();
        toolbar->setColorPreset(QString::fromStdString(m_heatmapSettings->settings().palettePreset));
        connect(m_heatmapSettings.get(), &heatmap::HeatmapSettingsModel::changed, toolbar, [this, toolbar] {
            toolbar->setColorPreset(QString::fromStdString(m_heatmapSettings->settings().palettePreset));
        });
    }
    if (dataProcessor) {
        QMetaObject::invokeMethod(dataProcessor, [dataProcessor, connected = m_connected] {
            dataProcessor->setRecordingConnected(connected);
        }, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::connectionStatusChanged,
                dataProcessor, &DataProcessor::setRecordingConnected, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::recordingViewError,
                dataProcessor, &DataProcessor::onRecordingViewError, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::recordingHeatmapLiveReceived,
                dataProcessor, &DataProcessor::onRecordingLiveReceived, Qt::QueuedConnection);
        connect(dataProcessor, &DataProcessor::recordingViewNeeded, this,
                [this](const recording::LiveView& view) {
                    if (canRequestConfiguredHistoryForSymbol(QString::fromStdString(view.symbol)))
                        m_dataSource->registerRecordingView(view);
                }, Qt::QueuedConnection);
        connect(dataProcessor, &DataProcessor::recordingViewReleased, this,
                [this](const recording::LiveView& view) {
                    if (m_dataSource) m_dataSource->releaseRecordingView(view);
                }, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::recordingHeatmapHistoryReceived,
                dataProcessor, &DataProcessor::onRecordingHistoryReceived, Qt::QueuedConnection);
        connect(m_dataSource.get(), &IGridDataSource::recordingHeatmapHistoryError,
                dataProcessor, &DataProcessor::onRecordingHistoryError, Qt::QueuedConnection);
        connect(dataProcessor, &DataProcessor::recordingHistoryFetchNeeded, this,
                [this](const protocol::recordingwire::Request& request) {
                    if (!canRequestConfiguredHistoryForSymbol(QString::fromStdString(request.symbol))) return;
                    m_dataSource->requestRecordingHeatmapHistory(request);
                }, Qt::QueuedConnection);
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
    
    connect(m_dataSource.get(), &IGridDataSource::heatmapSliceReceived, this,
            [this](const HeatmapSlice& slice) {
                if (slice.symbol == m_currentSymbol) m_heatmapReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
            });
    connect(m_dataSource.get(), &IGridDataSource::tradeReceived, this,
            [this](const Trade& trade) {
                if (QString::fromStdString(trade.product_id) != m_currentSymbol) return;
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                m_tradesReceivedAtMs = now;
                AgentApi::TradeRow row;
                row.receivedAtMs = now;
                row.selectionEpoch = m_agentApiSelectionEpoch;
                row.id = QString::fromStdString(trade.trade_id);
                row.side = trade.side == AggressorSide::Buy ? "buy" :
                           trade.side == AggressorSide::Sell ? "sell" : "unknown";
                row.price = trade.price;
                row.qty = trade.size;
                m_agentApiTradeTape.append(std::move(row));
            });
    connect(m_dataSource.get(), &IGridDataSource::liveOrderBookUpdated, this,
            [this](const QString& symbol, const std::vector<BookDelta>&) {
                if (symbol != m_currentSymbol) return;
                m_bookReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
                auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
                if (!renderer) return;
                thread_local std::vector<std::pair<uint32_t, double>> bids, asks;
                const auto view = m_dataSource->getDirectLiveOrderBook(symbol.toStdString())
                                      .captureDenseNonZero(bids, asks, 1, 16384);
                const double bid = view.bidLevels.empty() ? 0.0
                    : view.minPrice + view.bidLevels.front().first * view.tickSize;
                const double ask = view.askLevels.empty() ? 0.0
                    : view.minPrice + view.askLevels.front().first * view.tickSize;
                renderer->setLiveBookTop(bid, ask);
            });
    if (auto* remote = dynamic_cast<RemoteGridDataSource*>(m_dataSource.get())) {
        connect(remote->streamClient(), &SentinelStreamClient::candleBarUpdateReceived, this,
                [this](const QString& symbol, int64_t, int64_t, int64_t, const SentinelStreamClient::CandleBar&) {
                    if (symbol == m_currentSymbol) m_candlesReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
                });
        connect(remote->streamClient(), &SentinelStreamClient::candleBarClosedReceived, this,
                [this](const QString& symbol, int64_t, int64_t, int64_t, const SentinelStreamClient::CandleBar&) {
                    if (symbol == m_currentSymbol) m_candlesReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
                });
    }

    connect(m_dataSource.get(), &IGridDataSource::connectionStatusChanged,
            this, &MainWindowGPU::onConnectionStatusChanged);
    connect(m_dataSource.get(), &IGridDataSource::tpoHistoryChunkReceived, this,
            [this](const QString& symbol, const QString& requestId, qint64 timeframeMs, int sessionType,
                   qint64 lastEndMs, int columns) {
                sLog_Probe("tpo.history", "reply id=" << requestId << " lastEnd=" << lastEndMs
                           << " columns=" << columns);
                sendTpoHistoryPage(m_tpoPager.onChunk(symbol.toStdString(), requestId.toStdString(),
                                                      timeframeMs, sessionType,
                                                      QDateTime::currentMSecsSinceEpoch()));
            });
    connect(m_dataSource.get(), &IGridDataSource::tpoHistoryFailed, this,
            [this](const QString& symbol, const QString& requestId, const QString& message) {
                sLog_Warning("TPO history page failed: symbol=" << symbol << " id=" << requestId
                             << " message=" << message);
                sendTpoHistoryPage(m_tpoPager.onError(requestId.toStdString(),
                                                      QDateTime::currentMSecsSinceEpoch()));
            });

    connect(m_dataSource.get(), &IGridDataSource::subscriptionAcknowledged, this,
            [this](const QString& symbol) {
                const auto actions = m_symbolSubscriptions.acknowledged(symbol);
                const bool activatedMain = std::any_of(actions.begin(), actions.end(), [](const auto& action) {
                    return action.kind == SymbolSubscriptionManager::Action::Activate
                        && action.consumer == QLatin1String("main");
                });
                applySubscriptionActions(actions);
                // An existing lease has no Activate action on reconnect.
                if (!activatedMain && symbol.trimmed().toUpper() == m_currentSymbol.trimmed().toUpper()
                    && m_symbolSubscriptions.held("main") == m_currentSymbol) {
                    requestConfiguredHistoryForSymbol(m_currentSymbol);
                    requestTpoHistoryForSymbol(m_currentSymbol);
                }
            });
    connect(m_dataSource.get(), &IGridDataSource::subscriptionRefused, this,
            [this](const QString& symbol, int cap, const QString& message) {
                m_lastSubscriptionRefusal = {{"symbol", symbol}, {"maxConnections", cap}, {"message", message}};
                applySubscriptionActions(m_symbolSubscriptions.refused(symbol));
                if (symbol.trimmed().toUpper() == m_currentSymbol.trimmed().toUpper()
                    && m_symbolSubscriptions.held("main").isEmpty())
                    m_refusedSymbol = symbol.trimmed().toUpper();
                statusBar()->showMessage(message);
            });
    connect(m_dataSource.get(), &IGridDataSource::errorOccurred,
            this, [this](const QString& error) {
                sLog_Warning("DataSource error: error=" << error << " symbol=" << m_currentSymbol
                             << " connected=" << m_connected);
            });
}

void MainWindowGPU::onConnectionStatusChanged(bool connected) {
    const bool wasConnected = m_connected;
    sLog_Data("Server connection status: connected=" << connected << " wasConnected=" << m_connected
              << " symbol=" << m_currentSymbol << " userSubscribed=" << m_userSubscribed);
    if (m_statusBar) {
        if (connected) {
            m_statusBar->setConnectionStatus(true);
        } else {
            // Show "Connecting..." immediately on disconnect — we always attempt reconnect.
            m_statusBar->setConnectionConnecting();
        }
    }
    if (connected != m_connected) {
        ++m_agentApiSelectionEpoch;
        m_heatmapReceivedAtMs.reset();
        m_candlesReceivedAtMs.reset();
        m_bookReceivedAtMs.reset();
        m_tradesReceivedAtMs.reset();
        if (auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr) {
            renderer->resetLivePriceCenter();
        }
    }
    m_connected = connected;
    if (!connected) {
        m_serverConfigReady = false;
    }

    if (m_subscribeButton) {
        m_subscribeButton->setText(connected ? "Subscribe" : "Connect");
        m_subscribeButton->setEnabled(true);
    }

    if (!connected) {
        m_tpoPager.cancel();  // replies to in-flight pages are gone; reconnect restarts
    }
    if (connected && !wasConnected) {
        applySubscriptionActions(m_symbolSubscriptions.reconnect());
        if (m_offlineRequestedSymbol.isEmpty() && m_symbolInput)
            m_symbolInput->setText(m_currentSymbol);
        if (!m_offlineRequestedSymbol.isEmpty()) {
            const QString requested = m_offlineRequestedSymbol;
            m_offlineRequestedSymbol.clear();
            applySubscriptionActions(m_symbolSubscriptions.request("main", requested));
        } else if (!m_initialSubscriptionAttempted && !m_currentSymbol.isEmpty()
                   && m_symbolSubscriptions.held("main").isEmpty()) {
            m_initialSubscriptionAttempted = true;
            applySubscriptionActions(m_symbolSubscriptions.request("main", m_currentSymbol));
        }
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
    docks.heatmapTelemetryDock = m_heatmapTelemetryDock;
    docks.heatmapTelemetryVisible = m_heatmapSettings && m_heatmapSettings->settings().showTelemetry;
    return docks;
}

AgentApi::Metadata MainWindowGPU::agentApiMetadata() const {
    AgentApi::Metadata meta;
    meta.sessionId = m_agentApiSessionId;
    meta.symbol = m_currentSymbol;
    meta.selectionEpoch = m_agentApiSelectionEpoch;
    meta.observedAtMs = QDateTime::currentMSecsSinceEpoch();
    meta.stale = !m_connected;
    meta.coverage = "unknown";
    return meta;
}

AgentApi::StateSnapshot MainWindowGPU::agentApiStateSnapshot() const {
    AgentApi::StateSnapshot s;
    s.meta = agentApiMetadata();
    const auto frameStats = PerformanceMonitor::instance().frameStats();
    s.frameIdle = frameStats.idle;
    s.renderRateHz = frameStats.window.renderRateHz;
    if (frameStats.window.samples > 0) {
        s.frameP50Ms = frameStats.window.p50Ms;
        s.frameP95Ms = frameStats.window.p95Ms;
    }
    s.connected = m_connected;
    s.lastSubscriptionRefusal = m_lastSubscriptionRefusal;
    s.serverConfigReady = m_serverConfigReady;
    const auto& client = GuiConfigStore::instance().clientConfig();
    s.serverHost = QString::fromStdString(client.server.host);
    bool portOk = false;
    const int port = QString::fromStdString(client.server.port).toInt(&portOk);
    if (portOk && port > 0 && port <= 65535) s.serverPort = port;
    if (m_serverConfigReady)
        AgentApi::applyAdvertisedServerConfig(s, GuiConfigStore::instance().serverConfig());
    s.heatmapReceivedAtMs = m_heatmapReceivedAtMs;
    if (auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
        renderer && renderer->gpuHeatmapActive()) {
        // gpu mode: the newest live frame of the chart's symbol/timeframe.
        const qint64 at = renderer->gpuHeatmapLayer()->liveReceivedAtMs();
        s.heatmapReceivedAtMs = at > 0 ? std::optional<qint64>(at) : std::nullopt;
    }
    s.candlesReceivedAtMs = m_candlesReceivedAtMs;
    s.bookReceivedAtMs = m_bookReceivedAtMs;
    s.tradesReceivedAtMs = m_tradesReceivedAtMs;
    if (m_qmlController) {
        if (auto* renderer = m_qmlController->getUnifiedGridRenderer()) {
            s.heatmapLayer = renderer->heatmapLayerEnabled();
            s.footprintLayer = renderer->footprintLayerEnabled();
            s.tpoLayer = renderer->tpoLayerEnabled();
            s.tpoLayout = renderer->tpoLayout();
            s.tpoTheme = renderer->tpoTheme();
            s.volumeProfileLayer = renderer->volumeProfileLayerEnabled();
        }
    }
    if (m_modeController) s.candlesLayer = m_modeController->candlesEnabled();
    return s;
}

AgentApi::ViewportSnapshot MainWindowGPU::agentApiViewportSnapshot() const {
    AgentApi::ViewportSnapshot s;
    s.meta = agentApiMetadata();
    if (!m_qmlController) return s;
    auto* renderer = m_qmlController->getUnifiedGridRenderer();
    if (!renderer) return s;
    const int64_t tf = renderer->getCurrentTimeframe();
    if (tf > 0) {
        s.heatmapTimeframeMs = tf;
        s.candleTimeframeMs = tf; // v1 QML links candle cadence to renderer cadence.
    }
    s.followLive = renderer->autoScrollEnabled();
    s.autoScale = renderer->autoPriceScale();
    auto* view = renderer->getViewState();
    if (!view || !view->isTimeWindowValid()) return s;
    const qint64 start = view->getVisibleTimeStart();
    const qint64 end = view->getVisibleTimeEnd();
    const double low = view->getMinPrice();
    const double high = view->getMaxPrice();
    if (end > start) { s.startMs = start; s.endMs = end; }
    if (std::isfinite(low) && std::isfinite(high) && high > low) { s.priceMin = low; s.priceMax = high; }
    s.viewportVersion = view->getViewportVersion();
    if (renderer->width() > 0) s.widthPx = renderer->width();
    if (renderer->height() > 0) s.heightPx = renderer->height();
    return s;
}

QJsonObject MainWindowGPU::agentApiHeatmapSnapshot() const {
    const auto &s = m_heatmapSettings->settings();
    const QJsonValue missing(QJsonValue::Null);
    auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
    const bool gpu = renderer && renderer->gpuHeatmapActive();
    QJsonObject out{{"renderer", QString::fromStdString(s.renderer)}, {"activeRenderer", gpu ? "gpu" : "legacy"},
        {"tickMode", s.tickMode == heatmap::TickMode::Auto ? "auto" : "manual"},
        {"tickUnits", s.tickMode == heatmap::TickMode::Manual ? QJsonValue(qint64(s.manualTick)) : missing},
        {"offeredPresets", missing}, {"settled", missing}, {"drawnTimeframeMs", missing},
        {"drawnTickUnits", missing}, {"indicatorText", missing}, {"residentBytes", missing},
        {"gpuBytes", missing}, {"liveVersion", missing}, {"liveAgeMs", missing}, {"controllerStats", missing},
        {"settings", heatmap::settingsJson(s)}};
    out["ui"] = m_heatmapControls->uiState();
    if (m_heatmapDataService) {
        const auto stats = m_heatmapDataService->stats();
        out["service"] = QJsonObject{{"connected", m_heatmapDataService->connected()},
            {"availabilityReady", m_heatmapDataService->availability(m_currentSymbol.toStdString()).has_value()},
            {"committedCpuBytes", qint64(stats.committedCpuBytes)},
            {"chunkRequests", qint64(stats.fetcher.requests)}};
    } else out["service"] = missing;
    if (gpu) {
        // Live layer state replaces the S6a placeholders (plan section 5).
        const auto state = renderer->gpuHeatmapLayer()->state();
        for (auto it = state.begin(); it != state.end(); ++it) out[it.key()] = it.value();
        out["tickUnits"] = state["tickUnits"];
    }
    return out;
}

AgentApi::ControlApply MainWindowGPU::agentApiApplyControl(const QString& kind, const AgentApi::ControlBody& body) {
    AgentApi::ControlApply out;
    if (kind == "docks") {
        const bool persist = AgentHostMode::dockChangesPersist(body.persistDocks);
        out.data["visible"] = m_dockVisibility->apply(body.dockVisible, body.dockFocus, persist);
        out.data["persist"] = persist;
        out.viewportVersion = agentApiViewportSnapshot().viewportVersion.value_or(0);
        return out;
    }
    auto* renderer = m_qmlController ? m_qmlController->getUnifiedGridRenderer() : nullptr;
    if (!renderer) {
        out.status = 503; out.code = "chart_unavailable"; out.message = "Chart renderer is unavailable";
        return out;
    }
    if (kind == "heatmap/settings") {
        // The settings model is the single source of truth: its changed() signal
        // drives the chart (renderer flip, tick policy, palette), the toolbar, the
        // settings dialog and the telemetry dock (S6c).
        const auto error = m_heatmapSettings->apply(body.heatmapSettings, body.persistHeatmapSettings);
        if (!error.isEmpty()) {
            out.status = 422; out.code = "invalid_settings"; out.message = error;
            return out;
        }
        sLog_App("Heatmap settings applied chart=main renderer=" << m_heatmapSettings->settings().renderer
                 << " persist=" << body.persistHeatmapSettings);
        out.data = agentApiHeatmapSnapshot();
        out.data["persist"] = body.persistHeatmapSettings;
    } else if (kind == "input") {
        auto *view = m_heatmapDock ? m_heatmapDock->qquickView() : nullptr;
        out = m_agentInput.apply(view, renderer->mapRectToScene(renderer->boundingRect()), body.input);
        if (out.status != 200) return out;
    } else if (kind == "symbol") {
        if (!subscribeSymbol(body.symbol)) {
            out.status = 422; out.code = "invalid_symbol"; out.message = "Invalid symbol";
            return out;
        }
        out.data["symbol"] = body.symbol;
    } else if (kind == "timeframe") {
        selectTimeframe(static_cast<int>(body.timeframeMs));
        out.data["linked"] = true;
        out.data["heatmapTimeframeMs"] = body.timeframeMs;
        out.data["candleTimeframeMs"] = body.timeframeMs;
    } else if (kind == "viewport") {
        const auto current = agentApiViewportSnapshot();
        if (!current.startMs || !current.endMs || !current.priceMin || !current.priceMax) {
            out.status = 503; out.code = "viewport_unavailable"; out.message = "Chart viewport is not ready";
            return out;
        }
        const QString error = renderer->applyViewportRequest(
            {body.startMs, body.endMs, body.priceMin, body.priceMax, body.followLive, body.autoScale, body.fit});
        if (error == "fit_unavailable") {
            out.status = 409; out.code = "fit_unavailable";
            out.message = "Auto-fit needs the gpu renderer and known data (live anchor or price)";
            return out;
        }
        if (error == "auto_scale_unavailable") {
            out.status = 409; out.code = "auto_scale_unavailable";
            out.message = "The auto price scale needs the gpu renderer";
            return out;
        }
        if (!body.fit.isEmpty()) out.data["fit"] = body.fit;
        const auto after = agentApiViewportSnapshot();
        out.viewportVersion = after.viewportVersion.value_or(0);
        out.data["viewportVersion"] = QString::number(out.viewportVersion);
        out.data["followLive"] = after.followLive.value_or(false);
        out.data["autoScale"] = after.autoScale.value_or(false);
    } else if (kind == "layers") {
        auto* toolbar = m_heatmapDock ? m_heatmapDock->toolbar() : nullptr;
        for (auto it = body.layers.begin(); it != body.layers.end(); ++it) {
            const bool enabled = it.value().toBool();
            if (it.key() == "tpoLayout") { renderer->setTpoLayout(it.value().toString()); continue; }
            if (it.key() == "tpoTheme") { renderer->setTpoTheme(it.value().toString()); continue; }
            if (it.key() == "heatmap") { if (toolbar) emit toolbar->heatmapToggled(enabled); else renderer->setHeatmapLayerEnabled(enabled); }
            else if (it.key() == "footprint") { if (toolbar) emit toolbar->footprintToggled(enabled); else renderer->setFootprintLayerEnabled(enabled); }
            else if (it.key() == "tpo") { if (toolbar) emit toolbar->tpoToggled(enabled); else renderer->setTpoLayerEnabled(enabled); }
            else if (it.key() == "volumeProfile") { if (toolbar) emit toolbar->volumeProfileToggled(enabled); else renderer->setVolumeProfileLayerEnabled(enabled); }
            else if (it.key() == "candles" && m_modeController) m_modeController->setCandlesEnabled(enabled);
        }
        if (toolbar) toolbar->setLayerToggleStates(renderer->heatmapLayerEnabled(), renderer->footprintLayerEnabled(),
                                                   renderer->tpoLayerEnabled(), renderer->volumeProfileLayerEnabled());
        const auto state = agentApiStateSnapshot();
        out.data["layers"] = QJsonObject{{"heatmap", state.heatmapLayer.value_or(false)},
                                          {"candles", state.candlesLayer.value_or(false)},
                                          {"footprint", state.footprintLayer.value_or(false)},
                                          {"tpo", state.tpoLayer.value_or(false)},
                                          {"volumeProfile", state.volumeProfileLayer.value_or(false)},
                                          {"tpoLayout", state.tpoLayout.value_or(QString())},
                                          {"tpoTheme", state.tpoTheme.value_or(QString())}};
    }
    if (!out.viewportVersion) out.viewportVersion = agentApiViewportSnapshot().viewportVersion.value_or(0);
    return out;
}

std::optional<AgentApi::CandleSnapshot> MainWindowGPU::agentApiCandlesSnapshot(
    const AgentApi::ValidationResult& query) const {
    const auto* remote = dynamic_cast<const RemoteGridDataSource*>(m_dataSource.get());
    const auto* buffer = remote ? qobject_cast<const CandleSeriesBuffer*>(remote->candleBuffer()) : nullptr;
    if (!buffer) return std::nullopt;
    return AgentApi::captureCandles(*buffer, m_currentSymbol, query.startMs, query.endMs,
                                    query.timeframeMs, static_cast<size_t>(query.limit), agentApiMetadata());
}

AgentApi::BookSnapshot MainWindowGPU::agentApiBookSnapshot(int levels) const {
    AgentApi::Metadata meta = agentApiMetadata();
    if (!m_dataSource || m_currentSymbol.isEmpty() || !m_bookReceivedAtMs) {
        AgentApi::BookSnapshot out;
        out.meta = std::move(meta);
        return out;
    }
    const LiveOrderBook& book = m_dataSource->getDirectLiveOrderBook(m_currentSymbol.toStdString());
    return AgentApi::captureBook(book, static_cast<size_t>(levels), m_bookReceivedAtMs, std::move(meta));
}

AgentApi::TradesSnapshot MainWindowGPU::agentApiTradesSnapshot(qint64 windowMs, int limit) const {
    return m_agentApiTradeTape.snapshot(agentApiMetadata(), windowMs, static_cast<size_t>(limit));
}

void MainWindowGPU::toggleSidePanel() {
    // Hide every visible right-area dock, or restore exactly the ones that were hidden.
    if (m_sidePanelHidden.isEmpty()) {
        for (auto* dock : findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (dockWidgetArea(dock) == Qt::RightDockWidgetArea && dock->isVisible() && !dock->isFloating()) {
                m_sidePanelHidden.push_back(dock);
                dock->hide();
            }
        }
        sLog_App("ui: side panel hidden docks=" << m_sidePanelHidden.size());
    } else {
        for (const auto& dock : std::as_const(m_sidePanelHidden))
            if (dock) dock->show();
        sLog_App("ui: side panel restored docks=" << m_sidePanelHidden.size());
        m_sidePanelHidden.clear();
    }
}
