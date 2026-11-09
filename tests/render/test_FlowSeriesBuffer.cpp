#include <gtest/gtest.h>
#include "datasources/FlowSeriesBuffer.hpp"
#include "datasources/FlowBackfillState.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QCoreApplication>
#include <QEvent>

namespace {
using Client = SentinelStreamClient;
using Source = Client::FlowSource;
using Status = Client::FlowStatus;
using Decision = FlowSeriesBuffer::Merge;
Client::FlowRecord bar(uint32_t revision = 1, bool complete = false, Source source = Source::Live,
                       int64_t start = 60'000, int64_t tf = 60'000) {
    Client::FlowRecord out;
    out.bar.startMs = start; out.bar.timeframeMs = tf;
    out.revision = revision; out.complete = complete; out.source = source;
    return out;
}
Client::FlowBatch batch(Client::FlowRecord record = bar(), bool history = false) {
    Client::FlowBatch out;
    out.symbol = "BTC-USD"; out.timeframeMs = record.bar.timeframeMs; out.history = history;
    out.bars.push_back(record);
    return out;
}
Client::FlowBatch reply(const FlowBackfillState::Request& req, int64_t oldest, Status status = Status::Complete) {
    Client::FlowBatch b;
    b.history = true; b.id = req.id; b.symbol = req.symbol; b.timeframeMs = req.timeframeMs;
    b.oldestMs = oldest; b.status = status;
    return b;
}
}
TEST(FlowSeriesBuffer, RevisionsCompareOnlyWithinSourceAndCompleteWinsTies) {
    auto held = bar(10, true);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(11), 0), Decision::Replace);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(9), 0), Decision::Drop);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(10, false), 0), Decision::Drop);
    held.complete = false;
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(10, true), 0), Decision::Replace);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(10, false), 0), Decision::Replace);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(0, false, Source::Store), 120'000), Decision::Replace);
    held = bar(1, false, Source::Store);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(100, true), 0), Decision::Drop);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(2, false, Source::Store), 120'000), Decision::Replace);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(0, true, Source::Store), 120'000), Decision::Drop);
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(1, true, Source::Store), 120'000), Decision::Replace);
    held.complete = true;
    EXPECT_EQ(FlowSeriesBuffer::merge(&held, bar(1, false, Source::Store), 120'000), Decision::Drop);
}
TEST(FlowSeriesBuffer, FinalLiveDroppedButCrossingCoarseBucketRemainsLive) {
    EXPECT_EQ(FlowSeriesBuffer::merge(nullptr, bar(500, true), 120'000), Decision::Drop);
    EXPECT_EQ(FlowSeriesBuffer::merge(nullptr, bar(500, false), 90'000), Decision::Replace);
    FlowSeriesBuffer buffer;
    auto page = batch(bar(0, false), true); page.sealedThroughMs = 90'000;
    buffer.apply(page, 0);
    buffer.apply(batch(bar(1, false)), 0);
    ASSERT_NE(buffer.series(60'000), nullptr);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).revision, 1u);
    page.sealedThroughMs = 120'000; page.bars = {bar(0, true, Source::Store)};
    buffer.apply(page, 0);
    buffer.apply(batch(bar(999, true)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).source, Source::Store);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).revision, 0u);
}
TEST(FlowSeriesBuffer, WatermarkReplacesAndWithdrawsUnbackedFinalLiveBars) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(), 0);
    auto page = batch(); page.history = true; page.bars.clear(); page.sealedThroughMs = 120'000;
    buffer.apply(page, 0);
    EXPECT_TRUE(buffer.series(60'000)->bars.empty());
    page.sealedThroughMs = 30'000;
    buffer.apply(page, 0);
    buffer.apply(batch(), 0);
    EXPECT_EQ(buffer.sealedThroughMs(), 30'000);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 1u);
}
TEST(FlowSeriesBuffer, LossRefetchIsRateLimitedAndAuthoritativePageResolvesSameRevisionLoss) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar(4, true)), 0);
    EXPECT_EQ(buffer.apply(batch(bar(4, false)), 10), std::vector<int64_t>{60'000});
    EXPECT_TRUE(buffer.apply(batch(bar(4, false)), 11).empty());
    EXPECT_TRUE(buffer.apply(batch(bar(3, false)), 12).empty());
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Partial);
    EXPECT_EQ(buffer.apply(batch(bar(4, false)), 30'010).size(), 1u);
    auto page = batch(bar(4, false), true);
    buffer.apply(page, 30'011);
    EXPECT_FALSE(buffer.series(60'000)->bars.at(60'000).complete);
    EXPECT_TRUE(buffer.apply(batch(bar(4, false)), 60'020).empty());
}
TEST(FlowSeriesBuffer, CoverageMaskIsIndependentOfBarsAndExposesRefused) {
    FlowSeriesBuffer buffer;
    auto page = batch(bar(), true);
    page.ranges = {{0, 60'000, Status::Building}, {60'000, 120'000, Status::Partial},
                   {120'000, 180'000, Status::Refused}};
    buffer.apply(page, 0);
    const auto& mask = buffer.series(60'000)->coverage;
    ASSERT_EQ(mask.size(), 3u);
    EXPECT_EQ(mask.at(0).status, Status::Building);
    EXPECT_EQ(mask.at(60'000).status, Status::Partial);
    EXPECT_EQ(mask.at(120'000).status, Status::Refused);
    buffer.apply(batch(bar(3, true)), 0);
    EXPECT_EQ(mask.at(60'000).status, Status::Partial);
}
TEST(FlowBackfillState, EmptyPagesProgressByOldestAndStopAtZero) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(first->limit, 512); EXPECT_EQ(first->endMs, 599'000);
    EXPECT_FALSE(state.next(1));
    EXPECT_TRUE(state.accept(reply(*first, 88'000), 1));
    EXPECT_FALSE(state.next(100));
    const auto second = state.next(101); ASSERT_TRUE(second);
    EXPECT_EQ(second->endMs, 87'000); EXPECT_EQ(second->limit, 88);
    EXPECT_TRUE(state.accept(reply(*second, 0), 102));
    EXPECT_FALSE(state.next(1000));
}
TEST(FlowBackfillState, RefusalStopsAndBusyRetriesWithExponentialBackoff) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_TRUE(state.fail(first->id, true, 10));
    EXPECT_FALSE(state.next(1009));
    const auto retry = state.next(1010); ASSERT_TRUE(retry);
    EXPECT_NE(retry->id, first->id); EXPECT_EQ(retry->endMs, first->endMs);
    EXPECT_FALSE(state.next(5000));
    EXPECT_TRUE(state.fail(retry->id, true, 1011));
    EXPECT_FALSE(state.next(3010));
    const auto again = state.next(3011); ASSERT_TRUE(again);
    EXPECT_TRUE(state.accept(reply(*again, 88'000, Status::Refused), 3012));
    EXPECT_FALSE(state.next(100'000));
}
TEST(FlowBackfillState, SwitchRetainsFlightAndRejectsStaleReplyEvenAfterSwitchBack) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto old = state.next(0); ASSERT_TRUE(old);
    state.select("ETH-USD", 1000); state.setWindow(0, 10'000);
    EXPECT_FALSE(state.next(100));
    state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    EXPECT_FALSE(state.accept(reply(*old, 0), 101));
    const auto current = state.next(101); ASSERT_TRUE(current);
    EXPECT_NE(current->id, old->id);
    EXPECT_FALSE(state.accept(reply(*old, 0), 102));
    EXPECT_TRUE(state.inFlight());
}
TEST(FlowBackfillState, BuildingIsRetriedSeparatelyAndSubsecondIsNotRequested) {
    FlowBackfillState state; state.select("BTC-USD", 100); state.setWindow(0, 10'000);
    EXPECT_FALSE(state.next(0));
    state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    auto b = reply(*first, 0); b.ranges = {{2000, 3000, Status::Building}};
    EXPECT_TRUE(state.accept(b, 1));
    EXPECT_FALSE(state.next(30'000));
    const auto refresh = state.next(30'001); ASSERT_TRUE(refresh);
    EXPECT_EQ(refresh->endMs, 2000); EXPECT_EQ(refresh->limit, 1);
}

struct FlowDataSourceTest : testing::Test {
    using Json = nlohmann::json;
    int argc = 1;
    char name[16] = "flow-source";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    RemoteGridDataSource source{"127.0.0.1", "1"};
    Client& client() { return *source.streamClient(); }
    std::vector<Json> requests() {
        client().m_ioc.restart(); client().m_ioc.poll();
        std::vector<Json> out;
        for (const auto& payload : client().m_writeQueue) out.push_back(Json::parse(payload));
        client().m_writeQueue.clear(); return out;
    }
    void deliver() { QCoreApplication::sendPostedEvents(&source, QEvent::MetaCall); }
    int flowDelay() const { return source.m_flowTimer.remainingTime(); }
    void ready() {
        client().connected(); deliver();
        client().serverConfigReceived(ServerConfig{}); deliver();
    }
    void select(const QString& symbol = "BTC-USD", int64_t tf = 60'000) {
        source.subscribeFlowLive(symbol, tf);
        source.requestFlowHistory(symbol, tf, 60'000, 120'000);
    }
    void emitLive(quint64 id, const QString& symbol, int64_t tf = 60'000) {
        auto b = batch(bar(1, true, Source::Live, 60'000, tf)); b.id = id; b.symbol = symbol;
        client().flowBarsReceived(std::make_shared<const Client::FlowBatch>(b)); deliver();
    }
    void emitPage(const Json& request) {
        Client::FlowBatch b; b.history = true; b.symbol = QString::fromStdString(request["symbol"]);
        b.timeframeMs = request["timeframe_ms"]; b.id = request["req"]; b.oldestMs = 0;
        client().flowBarsReceived(std::make_shared<const Client::FlowBatch>(b)); deliver();
    }
};
TEST_F(FlowDataSourceTest, NoRequestBeforeConfigOrAfterDisconnect) {
    select(); EXPECT_TRUE(requests().empty());
    client().connected(); deliver(); select(); EXPECT_TRUE(requests().empty());
    client().serverConfigReceived(ServerConfig{}); deliver(); select();
    const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0]["type"], "flow_live_subscribe");
    EXPECT_EQ(sent[1]["type"], "flow_history_request");
    client().disconnected(); deliver(); select(); EXPECT_TRUE(requests().empty());
    client().connected(); deliver(); select(); EXPECT_TRUE(requests().empty());
}
TEST_F(FlowDataSourceTest, SymbolAndTimeframeSwitchUnsubscribeClearAndRejectStaleFrames) {
    ready(); select(); auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    const auto sub = sent[0]["sub"].get<quint64>();
    emitLive(sub, "BTC-USD"); ASSERT_NE(source.flowBuffer().series(60'000), nullptr);
    select("ETH-USD"); const auto changed = requests(); ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed[0]["type"], "flow_live_unsubscribe"); EXPECT_EQ(changed[0]["sub"], sub);
    EXPECT_EQ(changed[1]["symbol"], "ETH-USD");
    EXPECT_EQ(source.flowBuffer().series(60'000), nullptr);
    emitLive(sub, "BTC-USD"); EXPECT_EQ(source.flowBuffer().series(60'000), nullptr);
    emitPage(sent[1]); // releases old flight, starts the current selection
    const auto next = requests(); ASSERT_EQ(next.size(), 1u); EXPECT_EQ(next[0]["symbol"], "ETH-USD");
    emitLive(changed[1]["sub"], "ETH-USD"); ASSERT_NE(source.flowBuffer().series(60'000), nullptr);
    select("ETH-USD", 1000); const auto tf = requests(); ASSERT_EQ(tf.size(), 2u);
    EXPECT_EQ(tf[0]["type"], "flow_live_unsubscribe"); EXPECT_EQ(tf[1]["timeframe_ms"], 1000);
    EXPECT_EQ(source.flowBuffer().series(60'000), nullptr);
}

TEST(FlowSeriesBuffer, LossRefetchCannotOverwriteNewerLiveRevisionAndEmptyRefetchWithdrawsStaleBar) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar(4, true)), 0);
    ASSERT_FALSE(buffer.apply(batch(bar(4, false)), 1).empty());
    buffer.apply(batch(bar(5, true)), 2);
    auto old = batch(bar(4, false), true); old.ranges = {{60'000, 120'000, Status::Partial}};
    buffer.apply(old, 3);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).revision, 5u);
    EXPECT_TRUE(buffer.series(60'000)->bars.at(60'000).complete);
    buffer.apply(batch(bar(5, false)), 30'001);
    auto empty = old; empty.bars.clear();
    buffer.apply(empty, 30'002);
    EXPECT_TRUE(buffer.series(60'000)->bars.empty());
}
TEST(FlowBackfillState, PanningBackReRequestsEvictedCoverageAndCoalescesBuildingRefresh) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_TRUE(state.accept(reply(*first, 0), 1));
    state.setWindow(20'000, 30'000);
    const auto second = state.next(101); ASSERT_TRUE(second);
    EXPECT_TRUE(state.accept(reply(*second, 20'000), 102));
    state.setWindow(0, 10'000);
    const auto again = state.next(202); ASSERT_TRUE(again);
    EXPECT_EQ(again->endMs, 9000);
    auto page = reply(*again, 0);
    page.ranges = {{2000, 3000, Status::Building}, {3000, 4000, Status::Building}};
    EXPECT_TRUE(state.accept(page, 203));
    const auto refresh = state.next(30'203); ASSERT_TRUE(refresh);
    EXPECT_EQ(refresh->endMs, 3000); EXPECT_EQ(refresh->limit, 2);
}
TEST(FlowSeriesBuffer, WindowEvictionAllowsOlderPagesInsteadOfKeepingOnlyNewestBars) {
    FlowSeriesBuffer buffer;
    buffer.retainWindow(60'000, 60'000, 120'000);
    buffer.apply(batch(bar(1, true)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 1u);
    buffer.retainWindow(60'000, 0, 60'000);
    EXPECT_TRUE(buffer.series(60'000)->bars.empty());
    buffer.apply(batch(bar(1, true, Source::Live, 0)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 1u);
    buffer.apply(batch(bar(2, true)), 1);
    EXPECT_EQ(buffer.series(60'000)->bars.begin()->first, 0);
}

TEST(FlowSeriesBuffer, AdvancingSealRequestsStoreForLiveBucketsOutsideTheNewPage) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar(100, true)), 0);
    auto page = batch(bar(1, false, Source::Live, 120'000), true);
    page.sealedThroughMs = 120'000;
    page.ranges = {{120'000, 180'000, Status::Complete}};
    EXPECT_EQ(buffer.apply(page, 1), std::vector<int64_t>{60'000});
    EXPECT_FALSE(buffer.series(60'000)->bars.contains(60'000));
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Building);
    auto store = batch(bar(0, true, Source::Store), true);
    store.sealedThroughMs = 120'000; store.ranges = {{60'000, 120'000, Status::Complete}};
    EXPECT_TRUE(buffer.apply(store, 2).empty());
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).source, Source::Store);
}
TEST(FlowBackfillState, ProvisionalCompleteRangeRefreshesWhileViewportIsStationary) {
    FlowBackfillState state; state.select("BTC-USD", 60'000); state.setWindow(60'000, 120'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    auto b = reply(*first, 60'000);
    b.bars = {bar(1, false)};
    b.ranges = {{60'000, 120'000, Status::Complete}};
    EXPECT_TRUE(state.accept(b, 1));
    EXPECT_FALSE(state.next(30'000));
    const auto refresh = state.next(30'001); ASSERT_TRUE(refresh);
    EXPECT_EQ(refresh->endMs, 60'000); EXPECT_EQ(refresh->limit, 1);
}

TEST(FlowBackfillState, FutureRefreshDoesNotDelayPagingThrottleOrBusyRetry) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    auto b = reply(*first, 88'000); b.ranges = {{88'000, 89'000, Status::Building}};
    ASSERT_TRUE(state.accept(b, 1));
    EXPECT_FALSE(state.next(1));
    EXPECT_EQ(state.wakeAtMs(1), 101);
    const auto second = state.next(101); ASSERT_TRUE(second);
    EXPECT_TRUE(state.fail(second->id, true, 102));
    EXPECT_EQ(state.wakeAtMs(102), 1102);
    EXPECT_TRUE(state.next(1102));
}
TEST_F(FlowDataSourceTest, QueuedProvisionalPageArmsShortPagingTimerBeforeFutureRefresh) {
    ready(); select(); const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    auto b = batch(bar(1, false), true); b.id = sent[1]["req"]; b.oldestMs = 60'000;
    b.ranges = {{60'000, 120'000, Status::Partial}};
    client().flowBarsReceived(std::make_shared<const Client::FlowBatch>(b)); deliver();
    EXPECT_GE(flowDelay(), 0);
    EXPECT_LE(flowDelay(), 150); // QTimer may round a coarse 100 ms deadline.
}
