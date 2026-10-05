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
    h.setTransport(Transport::Reconnecting);
    EXPECT_EQ(h.snapshot().state, State::Reconnecting);
    h.setTransport(Transport::Connected);
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
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
