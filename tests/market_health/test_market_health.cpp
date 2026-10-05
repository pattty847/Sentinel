#include <gtest/gtest.h>
#include "models/MarketHealth.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include "widgets/StatusBar.hpp"
#include "widgets/HeatmapTelemetryDock.hpp"
#include "widgets/PaperTradingDock.hpp"
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QTemporaryDir>
#include <QSettings>
#include <QToolButton>
#include "themes/ThemeManager.hpp"
#include "themes/FontManager.hpp"
#include "protocol/SentinelStreamClientTransport.hpp"
#include "heatmap/ChunkFetcher.hpp"
#include "../servermodel/FakeChunkTransport.hpp"

using State = MarketHealth::State;
using Transport = MarketHealth::Transport;
namespace {
void ready(MarketHealth& h, const QString& symbol = "BTC-USD") {
    h.setTransport(Transport::Connected);
    h.subscriptionRequested(symbol);
    h.subscriptionAcknowledged(symbol);
    h.bookReceived(symbol, 1000);
    h.setActiveSymbol(symbol);
}
TEST(MarketHealth, StartupAckThenBookAndQuietMarket) {
    MarketHealth h;
    EXPECT_EQ(h.snapshot().state, State::Initializing);
    h.setActiveSymbol("BTC-USD");
    h.subscriptionRequested("BTC-USD");
    h.setTransport(Transport::Connected);
    EXPECT_EQ(h.snapshot().state, State::WaitingForSubscription);
    h.subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(h.snapshot().state, State::WaitingForBook);
    EXPECT_FALSE(h.snapshot().bookAgeMs);
    EXPECT_EQ(MarketHealth::ageText(h.snapshot().bookAgeMs), "Unknown");
    h.bookReceived("BTC-USD", 1000);
    EXPECT_EQ(h.snapshot("BTC-USD", 999999999).state, State::Live);
    EXPECT_EQ(h.snapshot("BTC-USD", 2500).bookAgeMs, 1500);
    EXPECT_FALSE(h.snapshot().heatmapAgeMs);
}
TEST(MarketHealth, PendingOrRefusedSwitchDoesNotRelabelOldData) {
    MarketHealth h; ready(h);
    h.subscriptionRequested("PEPE-USD");
    EXPECT_EQ(h.activeSymbol(), "BTC-USD");
    EXPECT_EQ(h.snapshot().state, State::Live);
    EXPECT_EQ(h.snapshot("PEPE-USD").state, State::WaitingForSubscription);
    EXPECT_FALSE(h.snapshot("PEPE-USD").bookAgeMs);
    h.subscriptionRefused("PEPE-USD", "Connection cap reached");
    EXPECT_EQ(h.snapshot("PEPE-USD").state, State::Unavailable);
    EXPECT_EQ(h.snapshot().state, State::Live);
    h.subscriptionRequested("PEPE-USD");
    h.subscriptionAcknowledged("PEPE-USD");
    h.setActiveSymbol("PEPE-USD");
    EXPECT_EQ(h.snapshot().state, State::WaitingForBook);
    EXPECT_FALSE(h.snapshot().bookAgeMs);
}
TEST(MarketHealth, ReconnectRequiresFreshAckAndSnapshot) {
    MarketHealth h; ready(h);
    h.heatmapReceived("BTC-USD", 1100);
    h.setTransport(Transport::Reconnecting);
    EXPECT_EQ(h.snapshot().state, State::Reconnecting);
    h.setTransport(Transport::Connected);
    h.subscriptionRequested("BTC-USD");
    EXPECT_EQ(h.snapshot("BTC-USD", 2000).bookAgeMs, 1000);
    EXPECT_EQ(h.snapshot("BTC-USD", 2000).heatmapAgeMs, 900);
    EXPECT_EQ(h.snapshot().state, State::WaitingForSubscription);
    h.subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(h.snapshot().state, State::WaitingForBook);
    h.bookReceived("BTC-USD", 2500, false);
    EXPECT_EQ(h.snapshot().state, State::WaitingForBook);
    h.bookReceived("BTC-USD", 3000);
    EXPECT_EQ(h.snapshot().state, State::Live);
    h.setTransport(Transport::Disconnected);
    EXPECT_EQ(h.snapshot().state, State::Disconnected);
}
TEST(MarketHealth, WithdrawalStaleRecoveryAndNoPerMessageNotifications) {
    MarketHealth h; ready(h);
    QSignalSpy spy(&h, &MarketHealth::changed);
    for (int i = 0; i < 100; ++i) h.bookReceived("BTC-USD", 2000 + i);
    EXPECT_EQ(spy.count(), 0);
    h.bookUnavailable("BTC-USD", "waiting_for_tick");
    EXPECT_EQ(h.snapshot().state, State::Unavailable);
    h.bookStale("BTC-USD");
    EXPECT_EQ(h.snapshot().state, State::Stale);
    h.bookReceived("BTC-USD", 4000);
    EXPECT_EQ(h.snapshot().state, State::Live);
    h.subscriptionReleased("BTC-USD");
    h.bookReceived("BTC-USD", 5000);
    h.subscriptionAcknowledged("BTC-USD");
    EXPECT_EQ(h.snapshot().state, State::WaitingForSubscription);
    EXPECT_FALSE(h.snapshot().bookAgeMs);
}
TEST(MarketHealth, StaleCannotRecoverFromADeltaEvenWithoutWithdrawal) {
    MarketHealth h; ready(h);
    h.bookStale("BTC-USD");
    h.bookReceived("BTC-USD", 2000, false);
    EXPECT_EQ(h.snapshot().state, State::Stale);
    EXPECT_EQ(h.snapshot("BTC-USD", 3000).bookAgeMs, 2000);
    h.bookReceived("BTC-USD", 2500, true);
    EXPECT_EQ(h.snapshot().state, State::Live);
    EXPECT_EQ(h.snapshot("BTC-USD", 3000).bookAgeMs, 500);
}
TEST(MarketHealthWidgets, VisiblePaperDetachImmediatelyWithdrawsHealth) {
    RemoteGridDataSource source("127.0.0.1", "1");
    source.streamClient()->connected();
    QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall);
    ready(*source.marketHealth());
    PaperTradingDock dock;
    dock.setDataSource(&source);
    dock.setSymbol("BTC-USD");
    dock.show(); QTest::qWait(20);
    auto* label = dock.findChild<QLabel*>("paperMarketHealth");
    ASSERT_TRUE(label);
    EXPECT_TRUE(label->text().startsWith("Live"));
    EXPECT_FALSE(label->toolTip().isEmpty());
    dock.setDataSource(nullptr);
    EXPECT_EQ(label->text(), "Unavailable");
    EXPECT_TRUE(label->toolTip().isEmpty());
}
TEST(MarketHealth, ChartLoadingPartialHoldAndHistoryOnlyAreDistinct) {
    MarketHealth h; ready(h);
    MarketHealth::ChartFacts chart;
    chart.symbol = "BTC-USD";
    chart.loading = true;
    chart.reason = "Fetching visible history";
    h.setChartState(chart);
    EXPECT_EQ(h.snapshot().state, State::WaitingForHistory);
    EXPECT_EQ(h.snapshot().reason, chart.reason);
    chart.loading = false; chart.partial = true;
    chart.coverage = "Visible history contains uncovered intervals";
    h.setChartState(chart);
    EXPECT_EQ(h.snapshot().state, State::HistoryPartial);
    chart.partial = false; chart.holding = true;
    h.setChartState(chart);
    EXPECT_EQ(h.snapshot().state, State::WaitingForHistory);
    chart.holding = false;
    h.setChartState(chart);
    // Historical browsing intentionally has no live heatmap subscription.
    EXPECT_EQ(h.snapshot().state, State::Live);
    EXPECT_FALSE(h.snapshot().heatmapAgeMs);
    chart.unavailable = true; h.setChartState(chart);
    EXPECT_EQ(h.snapshot().state, State::Unavailable);
    chart.symbol = "OTHER"; h.setChartState(chart);
    EXPECT_EQ(h.snapshot().state, State::Live);
}
TEST(MarketHealth, RemoteSourceRejectsOldBookAndWithdrawsUnavailable) {
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* client = source.streamClient();
    auto* health = source.marketHealth();
    auto deliver = [&] { QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall); };
    client->connected(); deliver();
    source.subscribe("BTC-USD");
    health->setActiveSymbol("BTC-USD");
    client->subscriptionAcknowledged("BTC-USD"); deliver();
    const auto generation = client->bookDeliveryGeneration("BTC-USD");
    const std::vector<OrderBookLevel> bids{{100, 1}}, asks{{101, 1}};
    client->snapshotReceived("BTC-USD", bids, asks, 0.1, generation, "ready", 10); deliver();
    ASSERT_EQ(health->snapshot().state, State::Live);
    client->snapshotReceived("BTC-USD", {}, {}, 0, generation - 1, "waiting_for_tick", 11); deliver();
    EXPECT_EQ(health->snapshot().state, State::Live);
    client->snapshotReceived("BTC-USD", {}, {}, 0, generation, "waiting_for_tick", 9); deliver();
    EXPECT_EQ(health->snapshot().state, State::Live);
    QSignalSpy bookSpy(&source, &IGridDataSource::liveOrderBookUpdated);
    client->snapshotReceived("BTC-USD", {}, {}, 0, generation, "waiting_for_tick", 11); deliver();
    EXPECT_EQ(health->snapshot().state, State::Unavailable);
    EXPECT_EQ(bookSpy.count(), 1); // empty withdrawal event remains intact
    client->snapshotReceived("BTC-USD", bids, asks, 0.1, generation, "ready", 12); deliver();
    EXPECT_EQ(health->snapshot().state, State::Live);
}
void drainHealth() {
    for (int i = 0; i < 12; ++i) QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
heatmap::ChunkFramePtr healthLiveFrame(quint64 revision, const std::string& source = "hmc2.deep") {
    using namespace heatmap;
    constexpr qint64 base = 1'800'000'000'000; // aligned hour
    auto frame = std::make_shared<ChunkFrame>();
    frame->kind = ChunkKind::LiveColumn;
    frame->key = {"BTC-USD", source, kMinuteMs, base};
    frame->state = {false, base, revision};
    NativeColumn column;
    column.grid = {1, 100, 100}; column.baseRow = 95; column.observedMs = 1000;
    column.coverage[0] = {{95, 105, 1000}}; column.coverage[1] = column.coverage[0];
    column.entries = {{0, 1024}};
    frame->columns = {"BTC-USD", "deep", kMinuteMs, base, base + kMinuteMs, {}, {}};
    frame->columns.columns.push_back({base, 1000, recording::kProvisional, {std::move(column)}});
    frame->columns.scannedRanges = {{base, base + kMinuteMs}};
    return frame;
}
TEST(MarketHealth, AcceptedHeatmapPathRejectsExpiredIdsWrongSourcesAndOldRevisions) {
    using namespace heatmap;
    RemoteGridDataSource source("127.0.0.1", "1");
    auto& client = *source.streamClient();
    protocol::SentinelStreamClientTransport adapter(client);
    ChunkStore store;
    ChunkFetcher fetcher(store, adapter);
    auto& h = *source.marketHealth();
    h.observeHeatmapFetcher(&fetcher);
    h.observeHeatmapFetcher(&fetcher); // repeated hookup must not duplicate delivery
    client.connected(); drainHealth();
    ready(h);
    protocol::chunkwire::Availability availability;
    availability.symbol = "BTC-USD"; availability.chunkWireVersion = kChunkWireVersion;
    protocol::chunkwire::SourceInfo info;
    info.id = "hmc2.deep";
    constexpr qint64 base = 1'800'000'000'000;
    info.levels.push_back({kMinuteMs, kHourMs, base, base, base});
    availability.sources.push_back(info);
    client.heatmapAvailabilityReceived(availability); drainHealth();
    fetcher.wantLive(1, "BTC-USD"); // first allocated wire id is 1
    fetcher.releaseLive(1);
    fetcher.wantLive(1, "BTC-USD"); // replacement wire id is 2
    QSignalSpy accepted(&fetcher, &ChunkFetcher::liveAccepted);
    QSignalSpy healthChanges(&h, &MarketHealth::changed);
    client.heatmapLiveReceived(1, healthLiveFrame(10)); drainHealth();
    EXPECT_FALSE(h.snapshot().heatmapAgeMs);
    client.heatmapLiveReceived(2, healthLiveFrame(10, "hmc2.near")); drainHealth();
    EXPECT_FALSE(h.snapshot().heatmapAgeMs);
    client.heatmapLiveReceived(2, healthLiveFrame(10));
    QCoreApplication::sendPostedEvents(&adapter, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(&fetcher, QEvent::MetaCall);
    ASSERT_EQ(accepted.count(), 1);
    EXPECT_FALSE(h.snapshot().heatmapAgeMs) << "Worker acceptance must queue GUI delivery";
    drainHealth();
    ASSERT_EQ(healthChanges.count(), 1);
    const auto acceptedAt = accepted.at(0).at(1).toLongLong();
    const auto sampleAt = acceptedAt + 10'000;
    EXPECT_EQ(h.snapshot("BTC-USD", sampleAt).heatmapAgeMs, 10'000);
    client.heatmapLiveReceived(2, healthLiveFrame(10));
    client.heatmapLiveReceived(2, healthLiveFrame(9)); drainHealth();
    EXPECT_EQ(accepted.count(), 1);
    EXPECT_EQ(h.snapshot("BTC-USD", sampleAt).heatmapAgeMs, 10'000);
    QTest::qWait(5);
    client.heatmapLiveReceived(2, healthLiveFrame(11)); drainHealth();
    ASSERT_EQ(accepted.count(), 2);
    EXPECT_GT(accepted.at(1).at(1).toLongLong(), acceptedAt);
    EXPECT_LT(*h.snapshot("BTC-USD", sampleAt).heatmapAgeMs, 10'000);
    fetcher.releaseLive(1);
    client.heatmapLiveReceived(2, healthLiveFrame(12)); drainHealth();
    EXPECT_EQ(accepted.count(), 2);
}
TEST(MarketHealth, HeatmapObserverReplacementAndDestructionFenceQueuedEvents) {
    heatmap::ChunkStore store1, store2;
    FakeChunkTransport transport1, transport2;
    auto first = std::make_unique<heatmap::ChunkFetcher>(store1, transport1);
    auto second = std::make_unique<heatmap::ChunkFetcher>(store2, transport2);
    MarketHealth h; ready(h);
    h.observeHeatmapFetcher(first.get());
    first->liveAccepted("BTC-USD", 1000);
    h.observeHeatmapFetcher(second.get());
    drainHealth();
    EXPECT_FALSE(h.snapshot().heatmapAgeMs);
    second->liveAccepted("BTC-USD", 2000);
    drainHealth();
    EXPECT_EQ(h.snapshot("BTC-USD", 3000).heatmapAgeMs, 1000);
    second->liveAccepted("BTC-USD", 2500);
    second.reset();
    drainHealth();
    EXPECT_EQ(h.snapshot("BTC-USD", 3000).heatmapAgeMs, 1000);
    h.observeHeatmapFetcher(first.get());
    first->liveAccepted("BTC-USD", 2600);
    h.observeHeatmapFetcher(nullptr);
    drainHealth();
    EXPECT_EQ(h.snapshot("BTC-USD", 3000).heatmapAgeMs, 1000);
}
TEST(MarketHealth, RemoteReconnectRequestRetainsAgesUntilFreshSnapshot) {
    RemoteGridDataSource source("127.0.0.1", "1");
    auto* client = source.streamClient();
    auto& h = *source.marketHealth();
    client->connected(); drainHealth();
    source.subscribe("BTC-USD");
    client->subscriptionAcknowledged("BTC-USD"); drainHealth();
    const std::vector<OrderBookLevel> bids{{100, 1}}, asks{{101, 1}};
    client->snapshotReceived("BTC-USD", bids, asks, 0.1,
        client->bookDeliveryGeneration("BTC-USD"), "ready", 1); drainHealth();
    h.heatmapReceived("BTC-USD", 1000);
    const auto sampleAt = QDateTime::currentMSecsSinceEpoch() + 1000;
    const auto before = h.snapshot("BTC-USD", sampleAt);
    client->disconnected(); drainHealth();
    client->connected(); drainHealth();
    source.subscribe("BTC-USD");
    auto after = h.snapshot("BTC-USD", sampleAt);
    EXPECT_EQ(after.state, State::WaitingForSubscription);
    EXPECT_EQ(after.bookAgeMs, before.bookAgeMs);
    EXPECT_EQ(after.heatmapAgeMs, before.heatmapAgeMs);
    client->subscriptionAcknowledged("BTC-USD"); drainHealth();
    EXPECT_EQ(h.snapshot("BTC-USD").state, State::WaitingForBook);
    client->snapshotReceived("BTC-USD", bids, asks, 0.1,
        client->bookDeliveryGeneration("BTC-USD"), "ready", 1); drainHealth();
    EXPECT_EQ(h.snapshot("BTC-USD").state, State::Live);
}
TEST(MarketHealthWidgets, StatusAndTelemetryStopPollingWhileHiddenAndKeepUnknowns) {
    MarketHealth health; ready(health);
    int reads = 0;
    health.setChartProvider([&] { ++reads; return MarketHealth::ChartFacts{}; });
    StatusBar bar;
    bar.setMarketHealth(&health);
    EXPECT_EQ(reads, 0);
    bar.show(); QTest::qWait(30);
    EXPECT_EQ(bar.findChild<QLabel*>("marketState")->text(), "Live");
    EXPECT_GT(reads, 0);
    bar.hide(); const int hiddenReads = reads;
    QTest::qWait(1100);
    EXPECT_EQ(reads, hiddenReads);
    HeatmapTelemetryDock dock;
    int polls = 0;
    dock.setMarketHealth(&health);
    dock.setProvider([&]() -> std::optional<QVariantMap> { ++polls; return QVariantMap{{"tick", 5.0}}; });
    EXPECT_EQ(polls, 0);
    dock.show(); QTest::qWait(30);
    EXPECT_EQ(dock.valueText("tick"), "$5");
    EXPECT_EQ(dock.valueText("residentBytes"), "Unknown");
    EXPECT_EQ(dock.valueText("holding"), "Unknown");
    EXPECT_TRUE(dock.frameHistory().empty());
    EXPECT_TRUE(dock.valueText("marketDetail").contains("Heatmap live receive age: Unknown"));
    dock.hide(); const auto hiddenPolls = polls;
    dock.refresh(); QTest::qWait(350);
    EXPECT_EQ(polls, hiddenPolls);
}
TEST(MarketHealthWidgets, TelemetryVisualFixtures) {
    const auto output = qEnvironmentVariable("SENTINEL_TELEMETRY_SHOTS");
    if (output.isEmpty()) GTEST_SKIP() << "Opt-in native widget screenshots require an authorized GUI slot";
    ASSERT_TRUE(QDir().mkpath(output));
    ThemeManager::instance().initializeDefaults();
    ThemeManager::instance().applyTheme("dark", qApp);
    FontManager::instance().initialize(qApp);
    MarketHealth health;
    ready(health);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    health.bookReceived("BTC-USD", now);
    health.heatmapReceived("BTC-USD", now);
    MarketHealth::ChartFacts facts;
    facts.symbol = "BTC-USD";
    facts.loading = false; facts.holding = false;
    facts.coverage = "Viewport history coverage unknown";
    health.setChartState(facts);
    HeatmapTelemetryDock dock;
    dock.setMarketHealth(&health);
    dock.setProvider([]() -> std::optional<QVariantMap> {
        return QVariantMap{{"mode", "Auto"}, {"tick", 5.0}, {"connection", "Fixture connected"},
            {"frameMs", 2.0}, {"frameP95Ms", 3.0}, {"loadingSlots", 0}, {"settled", true}};
    });
    auto save = [&](const QString& name) {
        QTest::qWait(80);
        return dock.grab().save(QDir(output).filePath(name + ".png"));
    };
    dock.resize(1920, 990); dock.show();
    ASSERT_TRUE(save("telemetry-collapsed-wide-fixture"));
    auto* toggle = dock.findChild<QToolButton*>(); ASSERT_NE(toggle, nullptr);
    toggle->setChecked(true);
    ASSERT_TRUE(save("telemetry-expanded-wide-fixture"));
    dock.resize(440, 720);
    ASSERT_TRUE(save("telemetry-expanded-narrow-fixture"));
    toggle->setChecked(false);
    ASSERT_TRUE(save("telemetry-collapsed-narrow-fixture"));
    dock.setProvider({}); dock.setMarketHealth(nullptr);
    ASSERT_TRUE(save("telemetry-unavailable-narrow-fixture"));
}

}
int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    Q_INIT_RESOURCE(sentinel_ui_fonts);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
