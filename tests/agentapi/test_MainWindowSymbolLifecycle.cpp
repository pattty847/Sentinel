#include "MainWindowGpu.h"
#include "config/GuiConfigStore.hpp"
#include "config/AgentHostMode.hpp"
#include "datasources/IGridDataSource.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include "datasources/CandleBackfillState.hpp"
#include "models/MarketHealth.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/WatchlistDock.hpp"
#include "mainwindow/QmlSceneController.h"
#include "render/heatmap/HeatmapDataService.hpp"
#include "render/heatmap/HeatmapGpuLayer.hpp"
#include "../servermodel/FakeChunkTransport.hpp"
#include "widgets/ChartDock.hpp"
#include "widgets/ServiceLocator.hpp"
#include "mainwindow/GuiApiServer.h"
#include "mainwindow/AgentApiCodec.hpp"
#include "mainwindow/ChartAgentPort.hpp"
#include "UnifiedGridRenderer.h"
#include "models/TimeAxisModel.hpp"
#include "models/PriceAxisModel.hpp"
#include "render/CandlestickBatched.hpp"
#include "render/CandlestickOverlayItem.hpp"
#include "render/AlgoOverlayRenderer.hpp"
#include "render/PaperTradeOverlayModel.hpp"
#include "render/PaperTradeOverlayRenderer.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QQuickView>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

struct MainWindowSymbolLifecyclePeer {
    static AgentApi::ViewportSnapshot viewport(const MainWindowGPU& window) { return window.chartAgentPort().snapshot(window.agentApiMetadata()); }
    static UnifiedGridRenderer* renderer(const MainWindowGPU& window) { return window.m_qmlController->getUnifiedGridRenderer(); }
    static void clearChartScene(MainWindowGPU& window) { window.m_qquickView->setSource(QUrl{}); }
    static void arrange(MainWindowGPU& window) { window.m_layoutOrchestrator->arrangeDefaultLayout(window.getDockWidgets()); }
    static bool subscriptionReady(const MainWindowGPU& window) { return window.m_userSubscribed; }
    static int retryInterval(const MainWindowGPU& window) { return window.m_heldRetryTimer->interval(); }
    static bool retryActive(const MainWindowGPU& window) { return window.m_heldRetryTimer->isActive(); }
    static bool switchTimeoutActive(const MainWindowGPU& window) { return window.m_symbolSwitchTimer->isActive(); }
    static int switchTimeoutInterval(const MainWindowGPU& window) { return window.m_symbolSwitchTimer->interval(); }
    static void fireSwitchTimeout(MainWindowGPU& window) {
        window.m_symbolSwitchTimer->stop();
        QMetaObject::invokeMethod(window.m_symbolSwitchTimer, "timeout", Qt::DirectConnection);
    }
    static quint16 apiPort(const MainWindowGPU& window) { return window.m_guiApiServer->port(); }
    static void fireRetry(MainWindowGPU& window) {
        window.m_heldRetryTimer->stop();
        QMetaObject::invokeMethod(window.m_heldRetryTimer, "timeout", Qt::DirectConnection);
    }
};

namespace {
QString isolatedSettingsPath;
void select(MainWindowGPU& window, QLineEdit* input, const QString& symbol);
void deliver(RemoteGridDataSource* source) { QCoreApplication::sendPostedEvents(source, QEvent::MetaCall); }
void acknowledge(RemoteGridDataSource* source, const QString& symbol) {
    source->streamClient()->subscriptionAcknowledged(symbol);
    deliver(source);
}
void book(RemoteGridDataSource* source, const QString& symbol, uint64_t version = 1) {
    const auto generation = source->streamClient()->bookDeliveryGeneration(symbol.toStdString());
    source->streamClient()->snapshotReceived(symbol, {{100, 1}}, {{101, 1}}, 0.1, generation, "ready", version);
    deliver(source);
}
QStandardItem* watchItem(WatchlistDock* rail, const QString& symbol) {
    auto* tree = rail->findChild<QTreeView*>("watchRows");
    auto* rows = qobject_cast<QStandardItemModel*>(tree->model());
    for (int i = 0; i < rows->rowCount(); ++i)
        if (rows->item(i)->data(WatchlistDock::TickerRole).toString() == symbol) return rows->item(i);
    return nullptr;
}

TEST(ChartAgentPin, StartupTimeframeNotificationOrder) {
    MainWindowGPU window;
    auto* renderer = MainWindowSymbolLifecyclePeer::renderer(window);
    ASSERT_TRUE(renderer);
    auto* root = renderer->parentItem();
    auto* overlay = root->findChild<CandlestickOverlayItem*>();
    ASSERT_TRUE(overlay);
    int rootTimeframeAtCandleChange = 0;
    QObject::connect(overlay, &CandlestickOverlayItem::timeframeMsChanged, &window, [&] {
        rootTimeframeAtCandleChange = root->property("currentActiveTimeframe").toInt();
    });
    renderer->view()->setTimeframe(60000);
    EXPECT_EQ(rootTimeframeAtCandleChange, 60000);
    EXPECT_EQ(overlay->timeframeMs(), 60000);
}

TEST(ChartAgentPin, ControlledStartupHistoryAndViewport) {
    const auto labelPx = qgetenv("SENTINEL_AXIS_LABEL_PX");
    const auto restoreLabelPx = qScopeGuard([&] {
        if (labelPx.isNull()) qunsetenv("SENTINEL_AXIS_LABEL_PX");
        else qputenv("SENTINEL_AXIS_LABEL_PX", labelPx);
    });
    qputenv("SENTINEL_AXIS_LABEL_PX", "13");
    MainWindowGPU window;
    auto* renderer = MainWindowSymbolLifecyclePeer::renderer(window);
    ASSERT_TRUE(renderer);
    auto* root = renderer->parentItem();
    auto* controller = renderer->findChild<chart::ChartViewController*>();
    ASSERT_TRUE(controller);
    // No window, network delivery or render timing enters this fixture.
    constexpr qint64 now = 1791782235000;
    controller->livePlacement().setTimingForTest([] { return now; }, {});
    renderer->view()->setTimeframe(60000);
    root->setSize({504, 931});
    auto* state = renderer->getViewState();
    const auto initialVersion = state->getViewportVersion();
    QSignalSpy commits(state, &GridViewState::viewportChanged);
    CandleBackfillState history;
    std::vector<std::pair<qint64, int>> requests;
    qint64 oldest = 0, clock = now;
    const auto updateHistory = [&] {
        history.setViewport("BTC-USD", 60000, state->getVisibleTimeStart(), state->getVisibleTimeEnd());
    };
    QObject::connect(renderer->view(), &chart::ChartViewQml::viewportChanged, &window, updateHistory);
    const auto page = [&] {
        clock += 100;
        const auto request = history.next(oldest, false, clock);
        if (!request) return;
        requests.emplace_back(request->endMs / 1000, request->limit);
        oldest = request->startMs;
        EXPECT_TRUE(history.accept("BTC-USD", 60000, request->startMs, request->endMs, oldest, clock));
        auto* buffer = qobject_cast<CandleSeriesBuffer*>(renderer->candleBuffer());
        ASSERT_TRUE(buffer);
        buffer->applyHistory("BTC-USD", 60000, {
            {oldest, oldest + 60000, 84000, 84100, 83900, 84000, 1, true},
            {request->endMs - 60000, request->endMs, 84000, 84100, 83900, 84000, 1, true}});
    };
    renderer->setLiveBookTop(84088.2, 84088.4);
    page();
    root->setSize({1150, 931});
    page();
    page();
    renderer->view()->setViewport(1791769440000, 1791777600000, 83000, 84600);
    page();
    EXPECT_EQ(commits.count(), 5);
    EXPECT_EQ(state->getViewportVersion() - initialVersion, 8U);
    EXPECT_EQ(requests, (std::vector<std::pair<qint64, int>>{{1791782280, 102}, {1791776160, 162}, {1791766440, 86}}));
}

TEST(ChartAgentPin, FixedSceneViewportBytes) {
    MainWindowGPU window;
    auto* renderer = MainWindowSymbolLifecyclePeer::renderer(window);
    ASSERT_TRUE(renderer);
    renderer->parentItem()->setSize({1000, 600});
    renderer->view()->setTimeframe(60000);
    renderer->view()->setAutoPriceScale(false);
    renderer->view()->enableAutoScroll(false);
    renderer->view()->setViewport(120000, 6120000, 100, 200);
    auto snapshot = MainWindowSymbolLifecyclePeer::viewport(window);
    snapshot.meta = {"fixed", "BTC-USD", 7, 123456, false, "unknown", false};
    const auto bytes = QJsonDocument(AgentApi::viewportJson(snapshot)).toJson(QJsonDocument::Compact);
    EXPECT_EQ(bytes, QByteArray(R"PIN({"data":{"autoScale":false,"candleTimeframeMs":60000,"drawn":null,"endMs":6120000,"followLive":false,"heatmapTimeframeMs":60000,"heightPx":570,"priceMax":200,"priceMin":100,"startMs":120000,"viewportVersion":"4","widthPx":930,"zoom":{"msPerPx":6451.612903225807,"pricePerPx":0.17543859649122806}},"meta":{"coverage":"unknown","observedAtMs":123456,"selectionEpoch":"7","sessionId":"fixed","source":"gui-cache","stale":false,"symbol":"BTC-USD","truncated":false},"ok":true})PIN")) << bytes.constData();
}

TEST(ChartAgentPin, StoredDrawnAndCompletedRevisionStaySeparate) {
    UnifiedGridRenderer renderer;
    renderer.setSize({800, 600});
    renderer.view()->setTimeframe(60000);
    renderer.view()->setAutoPriceScale(false);
    renderer.view()->enableAutoScroll(false);
    renderer.view()->setViewport(120000, 6120000, 100, 200);
    chart::PublishedFrame published;
    MappingFrameContext drawn;
    drawn.raster.valid = true;
    drawn.raster.drawnStartMs = 150000;
    drawn.raster.drawnEndMs = 6000000;
    drawn.raster.drawnMinPrice = 110;
    drawn.raster.drawnMaxPrice = 190;
    drawn.raster.rowPx = 2;
    drawn.raster.colPx = 3;
    drawn.raster.dpr = 2;
    published.publish(drawn);
    chart::FrameAck ack;
    int updates = 0;
    ChartAgentPort port(renderer.view()->controller(), published, ack, [&] { ++updates; });
    const auto snapshot = port.snapshot();
    EXPECT_EQ(snapshot.startMs, 120000);
    EXPECT_EQ(snapshot.priceMin, 100);
    ASSERT_TRUE(snapshot.drawn);
    EXPECT_EQ(QJsonDocument(AgentApi::viewportJson(snapshot)["data"].toObject()["drawn"].toObject()).toJson(QJsonDocument::Compact),
              QByteArray(R"({"colPx":3,"dpr":2,"endMs":6000000,"priceMax":190,"priceMin":110,"rowPx":2,"startMs":150000})"));
    AgentApi::ControlBody body;
    body.priceMin = 120; body.priceMax = 180;
    const auto applied = port.applyViewport(body);
    EXPECT_EQ(applied.status, 200);
    EXPECT_EQ(applied.viewportVersion, snapshot.viewportVersion.value() + 1);
    EXPECT_EQ(port.snapshot().drawn->priceMin, 110);
    port.setControlRevision(9, 7);
    EXPECT_EQ(updates, 1);
    FrameContext frame;
    ack.stamp(frame);
    EXPECT_EQ(frame.controlRevision, 9);
    EXPECT_EQ(frame.selectionEpoch, 7);
    EXPECT_EQ(frame.viewportVersion, applied.viewportVersion);
    ack.submitted(frame);
    EXPECT_EQ(port.rendered().first, 0);
    ack.rendered();
    EXPECT_EQ(port.rendered(), (std::pair<quint64, quint64>{9, frame.frameId}));
    EXPECT_EQ(ChartAgentPort{}.applyViewport(body).status, 503);
}

TEST(MainWindowHealthIntegration, AckRefusalSupersedeTimeoutAndReconnectAgreeAcrossConsumers) {
    MainWindowGPU window;
    auto* source = dynamic_cast<RemoteGridDataSource*>(ServiceLocator::dataSource());
    ASSERT_TRUE(source);
    source->streamClient()->disconnectFromServer(); // Unit client only: no recorder or GUI host.
    auto* health = source->marketHealth();
    auto* chart = window.findChild<ChartDock*>();
    auto* rail = window.findChild<WatchlistDock*>();
    auto* telemetry = window.findChild<HeatmapTelemetryDock*>();
    auto* status = window.findChild<StatusBar*>();
    ASSERT_TRUE(chart && rail && telemetry && status);
    EXPECT_TRUE(health->activeSymbol().isEmpty());
    ASSERT_TRUE(watchItem(rail, "BTC-USD"));
    EXPECT_FALSE(watchItem(rail, "BTC-USD")->text().startsWith("●"));
    EXPECT_TRUE(watchItem(rail, "ETH-USD")->toolTip().contains("unverified"));

    // Expose only the QWidget consumer in an offscreen test surface; never show
    // MainWindow/QQuickView or trigger its GUI/service startup path.
    status->setParent(nullptr);
    std::unique_ptr<StatusBar> statusOwner(status);
    status->show();
    emit telemetry->visibilityChanged(true); // Same exposure signal as a selected dock tab.
    const auto agreement = [&](const QString& symbol, MarketHealth::State state) {
        health->refreshChartState();
        telemetry->refresh();
        EXPECT_EQ(health->activeSymbol(), symbol);
        EXPECT_EQ(health->snapshot().state, state);
        EXPECT_EQ(status->findChild<QLabel*>("marketSymbol")->text(), symbol.isEmpty() ? "No symbol" : symbol);
        EXPECT_EQ(status->findChild<QLabel*>("marketState")->text(), MarketHealth::stateText(state));
        EXPECT_EQ(telemetry->valueText("marketHealth"), symbol + " · " + MarketHealth::stateText(state));
    };
    source->streamClient()->connected(); deliver(source);
    agreement({}, MarketHealth::State::Initializing);
    EXPECT_TRUE(watchItem(rail, "BTC-USD")->text().startsWith("…"));
    acknowledge(source, "BTC-USD");
    agreement("BTC-USD", MarketHealth::State::WaitingForBook);
    book(source, "BTC-USD");
    agreement("BTC-USD", MarketHealth::State::Live);
    EXPECT_TRUE(watchItem(rail, "BTC-USD")->text().startsWith("●"));
    const qint64 observed = QDateTime::currentMSecsSinceEpoch() - 500;
    window.heatmapDataService()->onData([fetcher = window.heatmapDataService()->fetcher(), observed] {
        emit fetcher->liveAccepted("BTC-USD", observed);
    });
    QCoreApplication::sendPostedEvents(health, QEvent::MetaCall);
    ASSERT_TRUE(health->snapshot("BTC-USD", observed + 1000).heatmapAgeMs);
    EXPECT_EQ(*health->snapshot("BTC-USD", observed + 1000).heatmapAgeMs, 1000);
    const auto bookAge = health->snapshot("BTC-USD", observed + 1000).bookAgeMs;

    select(window, chart->symbolInput(), "ETH-USD");
    agreement("BTC-USD", MarketHealth::State::Live);
    EXPECT_TRUE(watchItem(rail, "ETH-USD")->text().startsWith("…"));
    source->streamClient()->subscriptionRefused("ETH-USD", 8, "Connection cap reached"); deliver(source);
    agreement("BTC-USD", MarketHealth::State::Live);
    EXPECT_TRUE(watchItem(rail, "ETH-USD")->text().startsWith("!"));
    EXPECT_EQ(watchItem(rail, "ETH-USD")->toolTip(), "Connection cap reached");

    select(window, chart->symbolInput(), "ETH-USD");
    select(window, chart->symbolInput(), "SOL-USD");
    source->streamClient()->subscriptionRefused("ETH-USD", 8, "Obsolete refusal"); deliver(source);
    EXPECT_TRUE(watchItem(rail, "SOL-USD")->text().startsWith("…"));
    EXPECT_FALSE(rail->findChild<QLabel*>("watchStatus")->text().contains("Obsolete"));
    acknowledge(source, "ETH-USD"); // Superseded acknowledgement cannot activate it either.
    agreement("BTC-USD", MarketHealth::State::Live);
    EXPECT_TRUE(watchItem(rail, "SOL-USD")->text().startsWith("…"));
    MainWindowSymbolLifecyclePeer::fireSwitchTimeout(window);
    EXPECT_TRUE(watchItem(rail, "SOL-USD")->text().startsWith("!"));
    EXPECT_TRUE(watchItem(rail, "SOL-USD")->toolTip().contains("timed out"));
    agreement("BTC-USD", MarketHealth::State::Live);

    select(window, chart->symbolInput(), "DOGE-USD");
    source->streamClient()->disconnected(); deliver(source);
    agreement("BTC-USD", MarketHealth::State::Reconnecting);
    EXPECT_TRUE(watchItem(rail, "DOGE-USD")->toolTip().contains("disconnection"));
    EXPECT_EQ(health->snapshot("BTC-USD", observed + 1000).bookAgeMs, bookAge);
    EXPECT_EQ(health->snapshot("BTC-USD", observed + 1000).heatmapAgeMs, 1000);
    source->streamClient()->connected(); deliver(source);
    agreement("BTC-USD", MarketHealth::State::WaitingForSubscription);
    EXPECT_TRUE(watchItem(rail, "BTC-USD")->text().startsWith("…"));
    EXPECT_FALSE(rail->findChild<QLabel*>("watchStatus")->text().contains("disconnection"));
    acknowledge(source, "BTC-USD"); // Held lease: no Activate action or symbolChanged.
    agreement("BTC-USD", MarketHealth::State::WaitingForBook);
    book(source, "BTC-USD", 2);
    agreement("BTC-USD", MarketHealth::State::Live);
    select(window, chart->symbolInput(), "SOL-USD");
    acknowledge(source, "SOL-USD");
    agreement("SOL-USD", MarketHealth::State::WaitingForBook);
    EXPECT_TRUE(watchItem(rail, "SOL-USD")->text().startsWith("●"));
    EXPECT_FALSE(health->snapshot().heatmapAgeMs);
    EXPECT_FALSE(health->snapshot().bookAgeMs);
    emit telemetry->visibilityChanged(false);
    EXPECT_FALSE(telemetry->timer()->isActive());
}

TEST(MainWindowHealthIntegration, ChartProviderKeepsMissingOldAndUnrenderedFactsUnknownAndSurvivesDeletion) {
    MainWindowGPU window;
    auto* source = dynamic_cast<RemoteGridDataSource*>(ServiceLocator::dataSource());
    ASSERT_TRUE(source);
    source->streamClient()->disconnectFromServer();
    source->streamClient()->connected(); deliver(source);
    acknowledge(source, "BTC-USD");
    book(source, "BTC-USD");
    auto* health = source->marketHealth();
    auto* renderer = MainWindowSymbolLifecyclePeer::renderer(window);
    ASSERT_TRUE(renderer);
    auto* layer = renderer->gpuHeatmapLayer(); // the chart's only heatmap renderer (S8a)
    ASSERT_TRUE(layer && layer->controller());
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    EXPECT_TRUE(health->snapshot().coverage.contains("unknown"));

    // Use the actual asynchronous controller publication, with no recorded
    // availability. Only the render-thread output atoms are injected; no GPU is
    // available here, and the test never interprets these as visual proof.
    renderer->view()->setViewport(1'000'000, 1'600'000, 100, 102);
    const auto waitSnapshot = [&](const QString& symbol, int64_t tf) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 2000) {
            const auto snapshot = layer->snapshot();
            if (snapshot && snapshot->symbol == symbol.toStdString() && snapshot->tfMs == tf) return true;
            QTest::qWait(10);
        }
        return false;
    };
    ASSERT_TRUE(waitSnapshot("BTC-USD", layer->tfMs()));
    auto stats = layer->tileStatsPtr();
    // Deliberately inconsistent/retained renderer atoms must never invent
    // current GUI facts, even when frame counts look successful.
    stats->drawnTfMs.store(layer->tfMs());
    stats->holding.store(true);
    stats->loadingSlots.store(3);
    stats->partialSlots.store(1);
    stats->refusedSlots.store(2);
    for (uint64_t frames : {10, 12, 100}) {
        stats->frames.store(frames);
        stats->errors.fetch_add(1);
        health->refreshChartState();
        EXPECT_FALSE(health->snapshot().holding.has_value());
        EXPECT_FALSE(health->snapshot().loading.has_value());
        EXPECT_FALSE(health->snapshot().partial.has_value());
        EXPECT_EQ(health->snapshot().state, MarketHealth::State::Live);
        EXPECT_TRUE(health->snapshot().coverage.contains("unknown"));
        EXPECT_FALSE(health->snapshot().heatmapAgeMs);
    }
    renderer->view()->setViewport(1'100'000, 1'700'000, 100, 102);
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().partial.has_value()) << "the previous viewport cannot describe the new one";

    select(window, window.findChild<ChartDock*>()->symbolInput(), "SOL-USD");
    acknowledge(source, "SOL-USD");
    health->refreshChartState(); // Layer still holds BTC's snapshot until queued data publication.
    EXPECT_EQ(health->snapshot().symbol, "SOL-USD");
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    ASSERT_TRUE(waitSnapshot("SOL-USD", layer->tfMs()));
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().holding.has_value());
    const auto oldTf = layer->tfMs();
    renderer->view()->setTimeframe(int(oldTf * 5));
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    ASSERT_TRUE(waitSnapshot("SOL-USD", layer->tfMs()));
    health->refreshChartState();
    stats->frames.fetch_add(2);
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().holding.has_value()) << "old drawn timeframe cannot label the new one";
    layer->setActive(false);
    layer->setActive(true);
    stats->frames.store(0); // Renderer reset; no completed publication.
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    EXPECT_FALSE(health->snapshot().partial.has_value());
    renderer->layers()->setHeatmapEnabled(false); // no heatmap drawn: no chart facts
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_TRUE(health->snapshot().coverage.isEmpty());
    // Remove the QML scene through its owner; deleting one QML child directly
    // leaves the scene's sibling bindings pointing at a half-torn-down chart.
    QPointer<UnifiedGridRenderer> lifetime(renderer);
    MainWindowSymbolLifecyclePeer::clearChartScene(window);
    EXPECT_TRUE(lifetime.isNull());
    health->refreshChartState();
    EXPECT_FALSE(health->snapshot().holding.has_value());
    EXPECT_TRUE(health->snapshot().coverage.isEmpty());
}

TEST(MainWindowHealthIntegration, NewPublicationAndCapacityRefusalDoNotClaimRendererPreparation) {
    MainWindowGPU window;
    auto* source = dynamic_cast<RemoteGridDataSource*>(ServiceLocator::dataSource());
    ASSERT_TRUE(source);
    source->streamClient()->disconnectFromServer();
    source->streamClient()->connected(); deliver(source);
    acknowledge(source, "BTC-USD");
    book(source, "BTC-USD");
    auto* health = source->marketHealth();
    auto* renderer = MainWindowSymbolLifecyclePeer::renderer(window);
    FakeChunkTransport* transport = nullptr;
    heatmap::HeatmapDataService fixture([&](QObject*) {
        transport = new FakeChunkTransport;
        return transport;
    });
    renderer->setHeatmapService(&fixture);
    renderer->view()->setTimeframe(int(heatmap::kMinuteMs));
    constexpr auto tile = heatmap::tiles::kTileColumns * heatmap::kMinuteMs;
    constexpr auto start = ((recording::kHmc2MinMs / tile) + 3) * tile;
    renderer->view()->setViewport(start, start + 8 * tile, 100, 102);
    auto* layer = renderer->gpuHeatmapLayer();
    const auto publishAvailability = [&](int64_t end) {
        fixture.onData([&] {
            heatmap::ChunkAvailability a;
            a.symbol = "BTC-USD";
            a.chunkWireVersion = heatmap::kChunkWireVersion;
            protocol::chunkwire::SourceInfo info;
            info.id = "deep";
            info.latestGrid = protocol::chunkwire::GridInfo{};
            info.latestGrid->priceScale = 100;
            info.levels.push_back({heatmap::kMinuteMs, heatmap::kHourMs, end, start, end - heatmap::kMinuteMs});
            a.sources.push_back(std::move(info));
            transport->goOnline();
            transport->push(std::move(a));
        });
    };
    const auto waitForSnapshot = [&](auto predicate) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 2000) {
            const auto snapshot = layer->snapshot();
            if (snapshot && snapshot->symbol == "BTC-USD" && snapshot->tfMs == heatmap::kMinuteMs
                && predicate(*snapshot)) return true;
            QTest::qWait(10);
        }
        return false;
    };
    publishAvailability(start + 10 * tile);
    ASSERT_TRUE(waitForSnapshot([](const auto& s) { return !s.spans.empty() && s.refused.empty(); }));
    const auto previous = layer->snapshot();
    auto stats = layer->tileStatsPtr();
    stats->errors.store(7);
    stats->frames.store(100);
    stats->drawnTfMs.store(layer->tfMs());
    stats->holding.store(true);
    stats->loadingSlots.store(9);
    stats->partialSlots.store(9);
    // Establish, then cross, the retired frame-count fence while its error
    // count stays stable. The second refresh made the old provider accept the
    // injected flags as current for this snapshot.
    health->refreshChartState();
    stats->frames.store(103);
    health->refreshChartState();
    publishAvailability(start + 11 * tile); // Ordinary publication, same serial and viewport.
    ASSERT_TRUE(waitForSnapshot([&](const auto& s) { return s.version > previous->version; }));
    ASSERT_EQ(layer->snapshot()->serial, previous->serial);
    health->refreshChartState(); // The corresponding renderer preparation has not occurred.
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    EXPECT_FALSE(health->snapshot().partial.has_value());

    // Actual controller capacity rejection, not an injected refusal flag.
    auto budgets = heatmap::HeatmapBudgets{};
    budgets.decodedChunks = budgets.spanSources = 64 * 1024;
    budgets.cpuCeiling = 128 * 1024;
    ASSERT_TRUE(fixture.setBudgets(budgets));
    ASSERT_TRUE(waitForSnapshot([](const auto& s) { return !s.refused.empty(); }));
    stats->loadingSlots.store(0);
    health->refreshChartState();
    EXPECT_EQ(health->snapshot().partial, true);
    EXPECT_EQ(health->snapshot().state, MarketHealth::State::HistoryPartial);
    EXPECT_TRUE(health->snapshot().reason.contains("CPU capacity limit"));
    EXPECT_FALSE(health->snapshot().loading.has_value());
    EXPECT_FALSE(health->snapshot().holding.has_value());
    EXPECT_FALSE(health->snapshot().heatmapAgeMs);
    stats->loadingSlots.store(9); // Hatch counters cannot distinguish denied content from work.
    health->refreshChartState();
    EXPECT_EQ(health->snapshot().state, MarketHealth::State::HistoryPartial);
    EXPECT_FALSE(health->snapshot().loading.has_value());
    renderer->setHeatmapService(window.heatmapDataService());
}

TEST(MainWindowHealthIntegration, LocalHostPolicyRefusalClearsOptimisticRailPendingWithoutCatalogClaim) {
    struct HostScope {
        ~HostScope() {
            AgentHostMode::resetForTests();
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolatedSettingsPath);
            QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, isolatedSettingsPath);
        }
    } restore;
    QTemporaryDir host("/private/tmp/sentinel-health-host-XXXXXX");
    QString error;
    ASSERT_TRUE(AgentHostMode::activate(host.path(), {}, &error)) << error.toStdString();
    AgentHostMode::setSymbolAllowlist({"BTC-USD"});
    MainWindowGPU window;
    auto* source = dynamic_cast<RemoteGridDataSource*>(ServiceLocator::dataSource());
    ASSERT_TRUE(source);
    source->streamClient()->disconnectFromServer();
    source->streamClient()->connected(); deliver(source);
    acknowledge(source, "BTC-USD");
    book(source, "BTC-USD");
    auto* rail = window.findChild<WatchlistDock*>();
    auto* row = watchItem(rail, "ETH-USD");
    ASSERT_TRUE(row);
    QSignalSpy selected(rail, &WatchlistDock::symbolSelected);
    for (int attempt = 0; attempt < 2; ++attempt) {
        ASSERT_TRUE(QMetaObject::invokeMethod(rail, "onRowActivated", Qt::DirectConnection,
                                             Q_ARG(QModelIndex, row->index())));
        EXPECT_EQ(selected.size(), attempt + 1) << "a rejected row must remain selectable";
        EXPECT_TRUE(row->text().startsWith("!"));
        EXPECT_TRUE(row->toolTip().contains("host session's symbol policy"));
        EXPECT_TRUE(rail->findChild<QLabel*>("watchStatus")->text().contains("excluded"));
        EXPECT_TRUE(watchItem(rail, "BTC-USD")->text().startsWith("●"));
        EXPECT_EQ(source->marketHealth()->activeSymbol(), "BTC-USD");
        EXPECT_EQ(source->marketHealth()->snapshot().state, MarketHealth::State::Live);
        EXPECT_FALSE(MainWindowSymbolLifecyclePeer::switchTimeoutActive(window));
        EXPECT_TRUE(watchItem(rail, "SOL-USD")->toolTip().contains("unverified"));
    }
}

TEST(MainWindowHealthIntegration, DefaultLayoutPreservesNavigationRailCapAtNarrowAndWideWidths) {
    MainWindowGPU window;
    auto* rail = window.findChild<WatchlistDock*>();
    ASSERT_TRUE(rail);
    const int cap = rail->maximumWidth();
    EXPECT_EQ(cap, 280);
    for (int width : {960, 1920}) {
        window.resize(width, 800);
        MainWindowSymbolLifecyclePeer::arrange(window);
        EXPECT_EQ(rail->maximumWidth(), cap);
        EXPECT_GE(window.findChild<ChartDock*>()->minimumWidth(), 480);
    }
}

void connected(MainWindowGPU& window) {
    // These tests inject lifecycle events. Retire the constructor's real client
    // and deliver any already queued down before simulating transport-up.
    auto* source = dynamic_cast<RemoteGridDataSource*>(ServiceLocator::dataSource());
    ASSERT_TRUE(source);
    source->streamClient()->disconnectFromServer();
    deliver(source);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, true)));
}

void select(MainWindowGPU& window, QLineEdit* input, const QString& symbol) {
    input->setText(symbol);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onSubscribe", Qt::DirectConnection));
}

void waitForInput(QLineEdit* input, const QString& expected) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (input->text() != expected && elapsed.elapsed() < 7000)
        QTest::qWait(20);
    EXPECT_EQ(input->text(), expected);
}

QByteArray apiRequest(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = {}) {
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        QByteArray head = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
        if (method == "POST")
            head += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        socket.write(head + "\r\n" + body);
    });
    socket.connectToHost("127.0.0.1", port);
    timer.start(3000);
    loop.exec();
    return socket.readAll();
}

QJsonObject responseField(const QByteArray& response, const char* field) {
    return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object()
        .value(QLatin1String(field)).toObject();
}

struct LocalApiPort {
    ClientConfig saved = GuiConfigStore::instance().clientConfig();
    QTemporaryDir screenshots;
    quint16 port = 0;
    LocalApiPort() {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0)) return;
        port = reservation.serverPort();
        reservation.close();
        auto config = saved;
        config.gui.apiPort = port;
        config.gui.screenshotDir = screenshots.path().toStdString();
        GuiConfigStore::instance().setClientConfig(config);
    }
    ~LocalApiPort() { GuiConfigStore::instance().setClientConfig(saved); }
};

TEST(MainWindowSymbolLifecycle, AcknowledgementRefusalTimeoutAndReconnect) {
    MainWindowGPU window(nullptr, 40);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    ASSERT_NE(input, nullptr);
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_EQ(changes.size(), 1);
    emit source->bookSnapshotStaleChanged("BTC-USD", true);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Order book stale: BTC-USD"));
    emit source->bookSnapshotStaleChanged("ETH-USD", false);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Order book stale: BTC-USD"));
    emit source->bookSnapshotStaleChanged("BTC-USD", false);
    EXPECT_TRUE(window.statusBar()->currentMessage().isEmpty());

    select(window, input, "ETH-USD");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to ETH-USD"));
    emit source->subscriptionRefused("", 8, "Connection cap reached");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_EQ(window.statusBar()->currentMessage(), "Connection cap reached");

    select(window, input, "SOL-USD");
    emit source->subscriptionAcknowledged("sol-usd");
    ASSERT_EQ(changes.size(), 2);
    EXPECT_EQ(input->text(), "SOL-USD");

    select(window, input, "ADA-USD");
    waitForInput(input, "SOL-USD");
    EXPECT_EQ(changes.size(), 2);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ADA-USD timed out");

    select(window, input, "XRP-USD");
    select(window, input, "DOGE-USD");
    emit source->subscriptionRefused("XRP-USD", 8, "stale refusal");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to DOGE-USD"));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_EQ(input->text(), "SOL-USD");
    connected(window);
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying SOL-USD in 5 s"));
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    select(window, input, "SOL-USD"); // manual retry while the backoff is armed
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionAcknowledged("SOL-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_EQ(changes.size(), 2);
    EXPECT_TRUE(window.statusBar()->currentMessage().isEmpty());

    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 15000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("SOL-USD", 8, "Cap of 8 reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 60000);
}

TEST(MainWindowSymbolLifecycle, ServerDefaultWinsAfterStartupAckUntilUserSelection) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_EQ(changes.size(), 1);
    ServerConfig config;
    config.defaultSymbols = {"ETH-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_EQ(changes.size(), 1);
    emit source->subscriptionAcknowledged("ETH-USD");
    ASSERT_EQ(changes.size(), 2);
    select(window, input, "SOL-USD");
    config.defaultSymbols = {"AVAX-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "SOL-USD");
    emit source->subscriptionAcknowledged("SOL-USD");
    EXPECT_EQ(changes.size(), 3);
}

TEST(MainWindowSymbolLifecycle, UserSelectionBeforeDefaultAckWinsStartupRace) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window); // BTC is requested but not yet acknowledged
    select(window, input, "SOL-USD");
    ServerConfig config;
    config.defaultSymbols = {"ETH-USD"};
    GuiConfigStore::instance().setServerConfig(config);
    EXPECT_EQ(input->text(), "SOL-USD");
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(changes.size(), 0); // superseded startup ack must not activate
    emit source->subscriptionAcknowledged("SOL-USD");
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(input->text(), "SOL-USD");
}

TEST(MainWindowSymbolLifecycle, SupersededRefusalLeavesCurrentSwitchPending) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    select(window, input, "SOL-USD");
    emit source->subscriptionRefused("ETH-USD", 8, "stale refusal");
    EXPECT_EQ(input->text(), "SOL-USD");
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Switching to SOL-USD"));
}

TEST(MainWindowSymbolLifecycle, ReconnectReadinessAndRefusalBackoff) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    connected(window);
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 15000);
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 60000);
    select(window, input, "BTC-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
}

TEST(MainWindowSymbolLifecycle, MissingAckTimesOutAndRestoresInput) {
    MainWindowGPU window(nullptr, 40);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    waitForInput(input, "BTC-USD");
    EXPECT_EQ(changes.size(), 1);
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ETH-USD timed out");
}

TEST(MainWindowSymbolLifecycle, OfflineSelectionStartsDeadlineOnlyAfterConnect) {
    MainWindowGPU window(nullptr, 40);
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    select(window, input, "ETH-USD");
    EXPECT_EQ(input->text(), "ETH-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::switchTimeoutActive(window));
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Waiting to connect"));
    QTest::qWait(100);
    EXPECT_EQ(input->text(), "ETH-USD");
    connected(window);
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::switchTimeoutActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::switchTimeoutInterval(window), 40);
    waitForInput(input, "BTC-USD");
    EXPECT_EQ(window.statusBar()->currentMessage(), "Switch to ETH-USD timed out");
}

TEST(MainWindowSymbolLifecycle, AgentOperationSurvivesSameSymbolReentryAndReportsEachFailureCode) {
    LocalApiPort api;
    ASSERT_NE(api.port, 0);
    MainWindowGPU window(nullptr, 500);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    const quint16 port = MainWindowSymbolLifecyclePeer::apiPort(window);
    ASSERT_EQ(port, api.port);
    const auto post = [port](const char* symbol) {
        return apiRequest(port, "POST", "/api/v1/symbol", QByteArray("{\"symbol\":\"") + symbol + "\"}");
    };
    const auto get = [port](const QString& id) {
        return apiRequest(port, "GET", "/api/v1/operations/" + id.toUtf8());
    };

    const auto first = post("ETH-USD");
    ASSERT_TRUE(first.startsWith("HTTP/1.1 200")) << first.toStdString();
    const QString firstId = responseField(first, "data").value("operationId").toString();
    ASSERT_EQ(responseField(first, "data").value("status"), "pending");
    select(window, input, "ETH-USD");
    const auto stillPending = get(firstId);
    EXPECT_TRUE(stillPending.startsWith("HTTP/1.1 200"));
    EXPECT_EQ(responseField(stillPending, "data").value("status"), "pending");
    emit source->subscriptionAcknowledged("ETH-USD");
    const auto activated = get(firstId);
    EXPECT_TRUE(activated.startsWith("HTTP/1.1 200"));
    EXPECT_NE(responseField(activated, "data").value("status"), "superseded");

    const auto refused = post("SOL-USD");
    const QString refusedId = responseField(refused, "data").value("operationId").toString();
    emit source->subscriptionRefused("SOL-USD", 8, "Cap reached");
    const auto refusedReply = get(refusedId);
    EXPECT_TRUE(refusedReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(refusedReply, "error").value("code"), "connection_cap");

    const auto timedOut = post("ADA-USD");
    const QString timeoutId = responseField(timedOut, "data").value("operationId").toString();
    MainWindowSymbolLifecyclePeer::fireSwitchTimeout(window);
    const auto timeoutReply = get(timeoutId);
    EXPECT_TRUE(timeoutReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(timeoutReply, "error").value("code"), "switch_timeout");

    const auto interrupted = post("DOT-USD");
    const QString interruptedId = responseField(interrupted, "data").value("operationId").toString();
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    const auto disconnectedReply = get(interruptedId);
    EXPECT_TRUE(disconnectedReply.startsWith("HTTP/1.1 409"));
    EXPECT_EQ(responseField(disconnectedReply, "error").value("code"), "disconnected");
}

TEST(MainWindowSymbolLifecycle, RefusedReplacementRearmsRetryForUnconfirmedHeldSymbol) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    ASSERT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    select(window, input, "ETH-USD");
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
    emit source->subscriptionRefused("ETH-USD", 8, "Cap reached for ETH");
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying BTC-USD in 5 s"));
    MainWindowSymbolLifecyclePeer::fireRetry(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::subscriptionReady(window));
    EXPECT_FALSE(MainWindowSymbolLifecyclePeer::retryActive(window));
}

TEST(MainWindowSymbolLifecycle, TimedOutReplacementRearmsRetryForUnconfirmedHeldSymbol) {
    MainWindowGPU window(nullptr, 40);
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    connected(window);
    emit source->subscriptionRefused("BTC-USD", 8, "Cap reached");
    select(window, input, "ETH-USD");
    waitForInput(input, "BTC-USD");
    EXPECT_TRUE(MainWindowSymbolLifecyclePeer::retryActive(window));
    EXPECT_EQ(MainWindowSymbolLifecyclePeer::retryInterval(window), 5000);
    EXPECT_TRUE(window.statusBar()->currentMessage().contains("Retrying BTC-USD in 5 s"));
}

TEST(MainWindowSymbolLifecycle, DisconnectAbandonsPendingSwitch) {
    MainWindowGPU window;
    auto* source = ServiceLocator::dataSource();
    auto* dock = window.findChild<ChartDock*>();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dock, nullptr);
    auto* input = dock->symbolInput();
    QSignalSpy changes(&window, &MainWindowGPU::symbolChanged);
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    select(window, input, "ETH-USD");
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onConnectionStatusChanged", Qt::DirectConnection,
                                          Q_ARG(bool, false)));
    EXPECT_EQ(input->text(), "BTC-USD");
    connected(window);
    emit source->subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(input->text(), "BTC-USD");
    EXPECT_EQ(changes.size(), 1);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    qmlRegisterModule("Sentinel", 1, 0);
    qmlRegisterType<UnifiedGridRenderer>("Sentinel", 1, 0, "UnifiedGridRenderer");
    qmlRegisterType<TimeAxisModel>("Sentinel", 1, 0, "TimeAxisModel");
    qmlRegisterType<PriceAxisModel>("Sentinel", 1, 0, "PriceAxisModel");
    qmlRegisterType<AlgoOverlayRenderer>("Sentinel", 1, 0, "AlgoOverlayRenderer");
    qmlRegisterType<PaperTradeOverlayModel>("Sentinel", 1, 0, "PaperTradeOverlayModel");
    qmlRegisterType<PaperTradeOverlayRenderer>("Sentinel", 1, 0, "PaperTradeOverlayRenderer");
    qmlRegisterModule("Sentinel.Charts", 1, 0);
    qmlRegisterType<CandlestickBatched>("Sentinel.Charts", 1, 0, "CandlestickBatched");
    qmlRegisterType<CandlestickOverlayItem>("Sentinel.Charts", 1, 0, "CandlestickOverlayItem");
    QTemporaryDir settingsDir;
    isolatedSettingsPath = settingsDir.path();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    ClientConfig client;
    client.server.host = "127.0.0.1";
    client.server.port = "1"; // no owner recorder; signals below are deterministic
    client.gui.apiPort = 0;
    GuiConfigStore::instance().setClientConfig(client);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
