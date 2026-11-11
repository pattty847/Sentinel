#include <gtest/gtest.h>
#include "datasources/FlowSeriesBuffer.hpp"
#include "datasources/FlowBackfillState.hpp"
#include "chart/feeds/CvdFeed.hpp"
#include "chart/feeds/FlowFeed.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include <future>
#include "chart/core/CvdHistory.hpp"
#include "datasources/RemoteGridDataSource.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>

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
TEST(FlowSeriesBuffer, LiveOnlyBucketHasLocalCoverage) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar()), 0);
    ASSERT_NE(buffer.series(60'000), nullptr);
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Live);
    auto page = batch(bar(1, false, Source::Store), true);
    page.ranges = {{60'000, 120'000, Status::Partial}};
    buffer.apply(page, 1);
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Partial);
    buffer.clear();
    page.ranges = {{0, 120'000, Status::Partial}};
    buffer.apply(page, 2);
    buffer.apply(batch(bar(2, false)), 3);
    EXPECT_EQ(buffer.series(60'000)->coverage.size(), 1u);
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
    EXPECT_TRUE(state.fail(first->id, Client::FlowFailure::Retryable, 10));
    EXPECT_FALSE(state.next(1009));
    const auto retry = state.next(1010); ASSERT_TRUE(retry);
    EXPECT_NE(retry->id, first->id); EXPECT_EQ(retry->endMs, first->endMs);
    EXPECT_FALSE(state.next(5000));
    EXPECT_TRUE(state.fail(retry->id, Client::FlowFailure::Retryable, 1011));
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
    int64_t flowStart() const { return source.m_flowBackfill.windowStartMs(); }
    void load(const Client::FlowBatch& batch) { source.m_flowBuffer.apply(batch, 0); }
    int flowDelay() const { return source.m_flowTimer.remainingTime(); }
    void ready() {
        client().connected(); deliver();
        client().serverConfigReceived(ServerConfig{}); deliver();
    }
    void select(const QString& symbol = "BTC-USD", int64_t tf = 60'000) {
        source.subscribeFlowLive(symbol, tf);
        source.requestFlowHistory(symbol, tf, 60'000, 120'000);
    }
    void receiveWire(const Json& message) { client().handleMessage(message.dump()); }
    void drainWire() {
        std::promise<void> done;
        net::post(*client().m_decodePool, [&] { done.set_value(); });
        done.get_future().wait();
        QCoreApplication::sendPostedEvents(&client(), QEvent::MetaCall);
        deliver();
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
TEST_F(FlowDataSourceTest, FlowOnlyConnectionAndReconnectPreserveFootprintCells) {
    using namespace sentinel::flow;
    FlowBarBuilder builder(60'000, 0.5, 100, 100, 100);
    ASSERT_EQ(builder.add({"sell", 60'001, 100, 1.1, Side::Sell}), FlowBarBuilder::AddResult::Accepted);
    ASSERT_EQ(builder.add({"buy", 60'002, 100.5, 2.3, Side::Buy}), FlowBarBuilder::AddResult::Accepted);
    ASSERT_EQ(builder.add({"buy2", 60'003, 101, 2.7, Side::Buy}), FlowBarBuilder::AddResult::Accepted);
    const auto flowBar = builder.bar(60'000); ASSERT_TRUE(flowBar);
    const auto encoded = encodeFlowBar(*flowBar);
    FlowSeriesBuffer::Series expectedSeries;
    expectedSeries.bars[60'000].bar = *flowBar;
    MappingFrameContext frame;
    frame.activeTimeframeMs = 60'000; frame.surfaceBounds = {0, 0, 200, 180};
    frame.surfaceDpr = 1; frame.viewportValid = true;
    auto& r = frame.raster;
    r.valid = true; r.dpr = 1; r.tick = 1; r.tfMs = 60'000;
    r.rowPx = 18; r.rowPxF = 18; r.colPx = 100; r.colPxF = 100;
    r.drawnStartMs = 60'000; r.drawnEndMs = 180'000; r.drawnMinPrice = 95; r.drawnMaxPrice = 105;
    r.topF = 1890; r.topRowIndex = 1890; r.heightDev = 180; r.widthDev = 200;
    auto expected = std::make_unique<chart::FlowSnapshot>();
    ASSERT_TRUE(chart::FlowFeed::bin(*expected, expectedSeries, frame));
    ASSERT_EQ(expected->barCount, 1); ASSERT_EQ(expected->cellCount, 2);
    EXPECT_EQ(expected->cells[0].bidVol, 110); EXPECT_EQ(expected->cells[0].askVol, 230);
    EXPECT_EQ(expected->cells[1].askVol, 270);

    for (int connection = 0; connection < 2; ++connection) {
        client().connected(); deliver();
        receiveWire({{"type", "server_config"}, {"schema_version", protocol::SentinelProtocol::kServerConfigSchemaVersion}}); deliver();
        select();
        const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
        EXPECT_EQ(sent[0]["type"], "flow_live_subscribe");
        EXPECT_EQ(sent[1]["type"], "flow_history_request");
        receiveWire({{"type", "flow_bar"}, {"schema_version", 1}, {"sub", sent[0]["sub"]},
            {"symbol", "BTC-USD"}, {"timeframe_ms", 60'000}, {"bucket_start_ms", 60'000},
            {"price_scale", 100}, {"row_tick_units", 50}, {"revision", 1},
            {"source", "live"}, {"closed", true}, {"complete", true}, {"encoding", "FLB1/base64"},
            {"bar", QByteArray(reinterpret_cast<const char*>(encoded.data()), encoded.size()).toBase64().toStdString()}});
        drainWire();
        const auto* series = source.flowBuffer().series(60'000); ASSERT_NE(series, nullptr);
        ASSERT_EQ(series->bars.size(), 1u);
        EXPECT_EQ(series->bars.at(60'000).bar, *flowBar);
        auto actual = std::make_unique<chart::FlowSnapshot>();
        ASSERT_TRUE(chart::FlowFeed::bin(*actual, *series, frame));
        ASSERT_EQ(actual->barCount, expected->barCount);
        ASSERT_EQ(actual->cellCount, expected->cellCount);
        EXPECT_EQ(actual->bars[0].bidVol, expected->bars[0].bidVol);
        EXPECT_EQ(actual->bars[0].askVol, expected->bars[0].askVol);
        EXPECT_EQ(actual->bars[0].delta, expected->bars[0].delta);
        EXPECT_EQ(actual->bars[0].maxSideVol, expected->bars[0].maxSideVol);
        for (int i = 0; i < actual->cellCount; ++i) {
            EXPECT_EQ(actual->cells[i].row, expected->cells[i].row);
            EXPECT_EQ(actual->cells[i].span, expected->cells[i].span);
            EXPECT_EQ(actual->cells[i].bidVol, expected->cells[i].bidVol);
            EXPECT_EQ(actual->cells[i].askVol, expected->cells[i].askVol);
        }
        client().disconnected(); deliver();
        EXPECT_EQ(source.flowBuffer().series(60'000), nullptr);
    }
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
    EXPECT_TRUE(buffer.series(60'000)->bars.contains(60'000)); // bounded live tail
    buffer.apply(batch(bar(1, true, Source::Live, 0)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 2u);
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
    EXPECT_TRUE(state.fail(second->id, Client::FlowFailure::Retryable, 102));
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

TEST(FlowBackfillState, SlidingWindowRequestsOnlyOneUncoveredBucket) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first); ASSERT_EQ(first->limit, 512);
    EXPECT_TRUE(state.accept(reply(*first, 88'000), 1));
    state.setWindow(0, 601'000);
    const auto edge = state.next(101); ASSERT_TRUE(edge);
    EXPECT_EQ(edge->endMs, 600'000);
    EXPECT_EQ(edge->limit, 1);
}
TEST(FlowBackfillState, LiveCoveredEdgeDoesNotPageHistory) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    state.beginLive(0);
    state.markLiveEdge(0);
    state.markLiveEdge(599'000);
    state.observeLive(599'000, 0);
    EXPECT_FALSE(state.next(0));
    state.setWindow(0, 601'000);
    state.markLiveEdge(600'000);
    EXPECT_FALSE(state.next(100));
    const auto probe = state.next(30'000); ASSERT_TRUE(probe);
    EXPECT_TRUE(probe->probe);
    EXPECT_EQ(probe->limit, 1);
}
TEST(FlowBackfillState, ReturningToTrimmedLiveIntervalPagesTheGap) {
    FlowBackfillState state; state.select("BTC-USD", 60'000);
    state.setWindow(9 * 3'600'000 + 40 * 60'000, 10 * 3'600'000 + 31 * 60'000);
    state.beginLive(10 * 3'600'000);
    for (int minute = 0; minute <= 30; ++minute)
        state.markLiveEdge(10 * 3'600'000 + minute * 60'000);
    state.setWindow(7 * 3'600'000, 9 * 3'600'000);
    state.markLiveEdge(10 * 3'600'000 + 40 * 60'000);
    state.setWindow(9 * 3'600'000 + 40 * 60'000, 10 * 3'600'000 + 41 * 60'000);
    state.markLiveEdge(10 * 3'600'000 + 40 * 60'000);
    auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(first->endMs, 10 * 3'600'000 + 39 * 60'000);
    EXPECT_EQ(first->limit, 9); // gap above the retained three-bucket tail
    EXPECT_TRUE(state.accept(reply(*first, 10 * 3'600'000 + 31 * 60'000), 1));
    auto older = state.next(101); ASSERT_TRUE(older);
    EXPECT_EQ(older->endMs, 10 * 3'600'000 + 27 * 60'000);
    EXPECT_LE(older->endMs - older->limit * 60'000, 10 * 3'600'000);
}
TEST(FlowBackfillState, SilentSubscriptionCoversAdvancingEdge) {
    FlowBackfillState state; state.select("BTC-USD", 1000);
    state.setWindow(60'000, 61'000);
    state.beginLive(60'000);
    int requests = 0;
    for (int second = 0; second < 60; ++second) {
        state.setWindow(60'000, 61'000 + second * 1000);
        if (second % 5 == 0) state.markLiveEdge(60'000 + second * 1000);
        if (second == 0) EXPECT_EQ(state.wakeAtMs(0), 5000);
        if (auto req = state.next(second * 1000)) {
            ++requests;
            EXPECT_TRUE(state.accept(reply(*req, req->endMs), second * 1000 + 1));
        }
    }
    EXPECT_LE(requests, 13);
}
TEST(FlowBackfillState, LongLiveSessionUsesOneSealProbePerInterval) {
    FlowBackfillState state; state.select("BTC-USD", 60'000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    auto initial = reply(*first, 0);
    for (int i = 0; i < 10; ++i) initial.bars.push_back(bar(1, false, Source::Live, int64_t(i) * 60'000));
    EXPECT_TRUE(state.accept(initial, 1));
    int requests = 0;
    for (int i = 0; i < 10; ++i) {
        const int64_t now = (i + 1) * 30'001;
        state.observeLive((10 + i) * 60'000, now);
        state.setWindow(0, (11 + i) * 60'000);
        const auto probe = state.next(now); ASSERT_TRUE(probe);
        EXPECT_TRUE(probe->probe);
        EXPECT_EQ(probe->limit, 1);
        EXPECT_EQ(probe->endMs, (10 + i) * 60'000);
        auto b = reply(*probe, probe->endMs);
        b.bars = {bar(1, false, Source::Live, probe->endMs)};
        EXPECT_TRUE(state.accept(b, now + 1));
        ++requests;
    }
    EXPECT_EQ(requests, 10);
}
TEST(FlowSeriesBuffer, EqualRevisionHistoryLiveCannotOverwriteNewerStreamContent) {
    FlowSeriesBuffer buffer;
    auto stream = batch(bar(7, true));
    stream.bars[0].bar.quoteVolume = 10;
    buffer.apply(stream, 0);
    auto page = batch(bar(7, true), true);
    page.bars[0].bar.quoteVolume = 20;
    buffer.apply(page, 1);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).bar.quoteVolume, 10);
    buffer.clear();
    stream.bars[0].complete = false;
    buffer.apply(stream, 0);
    buffer.apply(page, 1);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).bar.quoteVolume, 20);
    EXPECT_TRUE(buffer.series(60'000)->bars.at(60'000).complete);
    page.history = false;
    page.bars[0].bar.quoteVolume = 30;
    buffer.apply(page, 2);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).bar.quoteVolume, 30);
}
TEST(FlowBackfillState, OversizedPageHalvesToOneThenMarksOnlyThatBucketRefused) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    auto request = state.next(0); ASSERT_TRUE(request);
    int64_t now = 0;
    for (int expected = 256; expected >= 1; expected /= 2) {
        EXPECT_TRUE(state.fail(request->id, Client::FlowFailure::Oversized, now));
        EXPECT_FALSE(state.next(now));
        now += 100;
        request = state.next(now); ASSERT_TRUE(request);
        EXPECT_EQ(request->limit, expected);
    }
    EXPECT_TRUE(state.fail(request->id, Client::FlowFailure::Oversized, now));
    EXPECT_EQ(state.takeRefusedBucket(), std::optional<int64_t>(599'000));
    now += 100;
    const auto older = state.next(now); ASSERT_TRUE(older);
    EXPECT_EQ(older->endMs, 598'000);
    EXPECT_EQ(older->limit, 1);
}
TEST(FlowBackfillState, PagingStopStillAllowsBuildingAndLossRefresh) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    state.refresh(2000, 1); // a pending loss re-fetch
    EXPECT_TRUE(state.fail(first->id, Client::FlowFailure::Fatal, 0));
    const auto loss = state.next(1); ASSERT_TRUE(loss);
    EXPECT_EQ(loss->endMs, 2000);
    EXPECT_TRUE(state.accept(reply(*loss, 2000, Status::Refused), 2));
    state.refresh(3000, 3); // a building range remains eligible after refusal
    const auto building = state.next(103); ASSERT_TRUE(building);
    EXPECT_EQ(building->endMs, 3000);
    EXPECT_FALSE(state.next(103));
}
TEST(FlowBackfillState, HostRefusalStopsSelectionAndAllRefreshes) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    state.beginLive(9000);
    state.markLiveEdge(9000);
    const auto request = state.next(0); ASSERT_TRUE(request);
    state.refresh(2000, 0);
    state.refuseSelection();
    EXPECT_FALSE(state.inFlight());
    EXPECT_FALSE(state.next(1));
    EXPECT_EQ(state.wakeAtMs(1), 0);
    state.refresh(3000, 2);
    state.observeLive(9000, 2);
    state.setWindow(0, 20'000);
    EXPECT_FALSE(state.next(2));
    EXPECT_FALSE(state.accept(reply(*request, 0), 3));
    state.select("ETH-USD", 1000);
    state.setWindow(0, 10'000);
    const auto other = state.next(10); ASSERT_TRUE(other);
    EXPECT_TRUE(state.fail(other->id, Client::FlowFailure::Retryable, 10));
    state.refuseSelection();
    EXPECT_EQ(state.wakeAtMs(11), 0);
    EXPECT_FALSE(state.next(1010));
}
TEST(FlowBackfillState, MissingReplyTimesOutAndMatchingLateReplyIsAppliedOnce) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(state.wakeAtMs(0), 5000);
    EXPECT_FALSE(state.timeout(4999));
    EXPECT_TRUE(state.timeout(5000));
    EXPECT_FALSE(state.next(5999));
    const auto retry = state.next(6000); ASSERT_TRUE(retry);
    EXPECT_NE(first->id, retry->id);
    EXPECT_TRUE(state.accept(reply(*first, 0), 6001));
    EXPECT_FALSE(state.inFlight());
    EXPECT_FALSE(state.accept(reply(*retry, 0), 6002));
}
TEST(FlowBackfillState, ColdSecondReplayHasLongTimeoutAndAcceptsLateReplyOnce) {
    FlowBackfillState state; state.select("BTC-USD", 1000);
    state.setWindow(0, 10'000);
    state.beginLive(40 * 60'000);
    auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(state.wakeAtMs(0), 30'000);
    EXPECT_FALSE(state.timeout(5'000));
    EXPECT_TRUE(state.timeout(30'000));
    EXPECT_TRUE(state.accept(reply(*first, 0), 30'001));
    EXPECT_FALSE(state.accept(reply(*first, 0), 30'002));
    EXPECT_FALSE(state.next(31'101));
}
TEST(FlowBackfillState, OversizedCoalescedRefreshPreservesOlderBuckets) {
    FlowBackfillState state; state.select("BTC-USD", 1000);
    for (int i = 0; i < 8; ++i) state.refresh(i * 1000, 0);
    auto first = state.next(0); ASSERT_TRUE(first);
    ASSERT_EQ(first->limit, 8);
    EXPECT_EQ(first->endMs, 7000);
    EXPECT_TRUE(state.fail(first->id, Client::FlowFailure::Oversized, 0));
    auto smaller = state.next(100); ASSERT_TRUE(smaller);
    EXPECT_EQ(smaller->limit, 4);
    EXPECT_EQ(smaller->endMs, 7000);
    EXPECT_TRUE(state.accept(reply(*smaller, 4000), 101));
    auto older = state.next(201); ASSERT_TRUE(older);
    EXPECT_EQ(older->endMs, 3000);
    EXPECT_EQ(older->limit, 4);
}
TEST(FlowSeriesBuffer, KeepsOnlyNewestThreeLiveBucketsBeyondViewport) {
    FlowSeriesBuffer buffer;
    buffer.retainWindow(60'000, 0, 120'000);
    for (int i = 2; i <= 5; ++i) buffer.apply(batch(bar(1, false, Source::Live, i * 60'000)), i);
    const auto* held = buffer.series(60'000); ASSERT_NE(held, nullptr);
    ASSERT_EQ(held->bars.size(), 3u);
    EXPECT_EQ(held->bars.begin()->first, 180'000);
    buffer.retainWindow(60'000, 0, 360'000);
    EXPECT_EQ(held->bars.size(), 3u);
    EXPECT_TRUE(held->bars.contains(300'000));
}

TEST_F(FlowDataSourceTest, DroppedLiveFrameMarksPartialAndSchedulesOneBucketRefetch) {
    ready(); select(); const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    const auto sub = sent[0]["sub"].get<quint64>();
    client().flowLiveDropped(sub, "BTC-USD", 60'000, 60'000); deliver();
    ASSERT_NE(source.flowBuffer().series(60'000), nullptr);
    EXPECT_EQ(source.flowBuffer().series(60'000)->coverage.at(60'000).status, Status::Partial);
    emitPage(sent[1]);
    QEventLoop loop;
    QTimer::singleShot(250, &loop, &QEventLoop::quit);
    loop.exec(); // the pager's 100 ms send throttle must expire
    const auto refetch = requests(); ASSERT_EQ(refetch.size(), 1u);
    EXPECT_EQ(refetch[0]["end_time_ms"], 60'000);
    EXPECT_EQ(refetch[0]["limit"], 1);
}

TEST_F(FlowDataSourceTest, HistoryFlightArmsFiveSecondTimeout) {
    ready(); select();
    const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    EXPECT_GE(flowDelay(), 4700);
    EXPECT_LE(flowDelay(), 5300);
}
TEST_F(FlowDataSourceTest, OversizedOneBucketReplyExposesRefusedCoverage) {
    ready();
    source.subscribeFlowLive("BTC-USD", 60'000);
    source.requestFlowHistory("BTC-USD", 60'000, 0, 120'000);
    const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    ASSERT_EQ(sent[1]["limit"], 2);
    client().flowHistoryFailed(sent[1]["req"].get<quint64>(),
        "flow history frame exceeds 15 MiB cap; reduce limit", Client::FlowFailure::Oversized);
    deliver();
    QEventLoop loop;
    QTimer::singleShot(250, &loop, &QEventLoop::quit);
    loop.exec();
    const auto retry = requests(); ASSERT_EQ(retry.size(), 1u);
    EXPECT_EQ(retry[0]["end_time_ms"], 60'000);
    EXPECT_EQ(retry[0]["limit"], 1);
    client().flowHistoryFailed(retry[0]["req"].get<quint64>(),
        "flow history frame exceeds 15 MiB cap; reduce limit", Client::FlowFailure::Oversized);
    deliver();
    const auto* held = source.flowBuffer().series(60'000); ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->coverage.at(60'000).status, Status::Refused);
}

TEST(FlowSeriesBuffer, RefusedHistoryDoesNotRetreatSealWatermark) {
    FlowSeriesBuffer buffer;
    auto sealed = batch(bar(3, true, Source::Store), true);
    sealed.sealedThroughMs = 120'000;
    buffer.apply(sealed, 0);
    auto refused = sealed;
    refused.status = Status::Refused;
    refused.sealedThroughMs = 0;
    refused.bars.clear();
    refused.ranges = {{0, 60'000, Status::Refused}};
    buffer.apply(refused, 1);
    EXPECT_EQ(buffer.sealedThroughMs(), 120'000);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).source, Source::Store);
    EXPECT_EQ(buffer.series(60'000)->coverage.at(0).status, Status::Refused);
}

namespace {
Client::FlowBatch cvdHistory(int64_t tf, int64_t end) {
    Client::FlowBatch b; b.symbol = "BTC-USD"; b.timeframeMs = tf;
    for (int64_t time = 0; time < end; time += tf) {
        auto record = bar(1, true, Source::Store, time, tf);
        record.bar.baseScale = 1; record.bar.delta = 1;
        b.bars.push_back(record);
    }
    return b;
}
MappingFrameContext cvdFrame(int64_t tf, int64_t start, int64_t end) {
    MappingFrameContext frame;
    frame.activeTimeframeMs = tf; frame.raster.valid = true;
    frame.raster.drawnStartMs = double(start); frame.raster.drawnEndMs = double(end);
    return frame;
}
}
TEST_F(FlowDataSourceTest, CvdResetAnchorRequestsAndRetainsMidnightAcrossPans) {
    constexpr int64_t hour = 3600000, tf = 60000;
    ready(); source.subscribeFlowLive("BTC-USD", tf);
    const auto anchor = chart::cvdHistoryAnchor(12 * hour, tf, 96, chart::CvdReset::UtcDay, 4);
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour, anchor);
    EXPECT_EQ(flowStart(), 0);
    load(cvdHistory(tf, 15 * hour));
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, *source.flowBuffer().series(tf), cvdFrame(tf, 12 * hour, 15 * hour), chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since UTC day");
    ASSERT_EQ(snapshot.series.count, 180); EXPECT_EQ(snapshot.series.points[60].value, 781);
    source.requestFlowHistory("BTC-USD", tf, 13 * hour, 16 * hour, anchor);
    EXPECT_EQ(source.flowBuffer().series(tf)->bars.begin()->first, 0);
    chart::CvdFeed::build(snapshot, *source.flowBuffer().series(tf), cvdFrame(tf, 13 * hour, 16 * hour), chart::CvdReset::UtcDay, 4);
    EXPECT_EQ(snapshot.series.points[0].value, 781); EXPECT_STREQ(snapshot.label.data(), "CVD since UTC day");
    // Off has exactly the original one-span margin and retires the extra history.
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour,
        chart::cvdHistoryAnchor(12 * hour, tf, 0, chart::CvdReset::UtcDay, 4));
    EXPECT_EQ(flowStart(), 9 * hour); EXPECT_EQ(source.flowBuffer().series(tf)->bars.begin()->first, 9 * hour);
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour,
        chart::cvdHistoryAnchor(12 * hour, tf, 96, chart::CvdReset::Visible, 4));
    EXPECT_EQ(flowStart(), 9 * hour); EXPECT_EQ(source.flowBuffer().series(tf)->bars.begin()->first, 9 * hour);
}
TEST_F(FlowDataSourceTest, CvdSecondBarsRespectCapAndLabelActualLoadedTime) {
    constexpr int64_t hour = 3600000, tf = 1000;
    ready(); source.subscribeFlowLive("BTC-USD", tf);
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour, 0);
    const auto expected = 15 * hour - int64_t(FlowSeriesBuffer::kMaxBuckets) * tf;
    EXPECT_EQ(flowStart(), expected);
    load(cvdHistory(tf, 15 * hour));
    const auto* series = source.flowBuffer().series(tf);
    ASSERT_EQ(series->bars.size(), FlowSeriesBuffer::kMaxBuckets); EXPECT_EQ(series->bars.begin()->first, expected);
    chart::CvdSnapshot snapshot;
    chart::CvdFeed::build(snapshot, *series, cvdFrame(tf, 12 * hour, 15 * hour), chart::CvdReset::UtcDay, 4);
    EXPECT_STREQ(snapshot.label.data(), "CVD since 12:43 loaded");
}
TEST(FlowSeriesBuffer, RetentionKeepsCoverageCrossingAnAnchor) {
    FlowSeriesBuffer buffer;
    buffer.setCoverage("BTC-USD", 60000, {0, 3600000, Status::Complete});
    buffer.retainWindow(60000, 1800000, 7200000);
    const auto* series = buffer.series(60000);
    ASSERT_NE(series, nullptr); ASSERT_EQ(series->coverage.size(), 1u);
    EXPECT_EQ(series->coverage.begin()->second.fromMs, 1800000);
    EXPECT_EQ(series->coverage.begin()->second.toMs, 3600000);
}

// The retention warning counts the buckets a window touches with the backfill's own
// endpoints, so it fires exactly when the backfill window drops the oldest bucket.
TEST(FlowSeriesBufferCapacity, TouchedBucketsMatchWhatBackfillRetains) {
    constexpr int64_t tf = 60000, cap = int64_t(FlowSeriesBuffer::kMaxBuckets);
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(0, cap * tf, tf), cap);
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(0, cap * tf + 1, tf), cap + 1);
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(0, cap * tf + tf / 2, tf), cap + 1) << "aligned start, 8194.5 tf";
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(tf / 2, cap * tf + tf / 2, tf), cap + 1) << "fractional both ends";
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(tf / 2, (cap - 1) * tf + tf / 2, tf), cap);
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(-tf / 2, tf / 2, tf), 2) << "floor below zero";
    EXPECT_EQ(FlowSeriesBuffer::bucketsTouching(5, 5, tf), 0);
    struct Window { int64_t start, end; };
    const int64_t base = 1000 * tf;
    for (const auto& w : {Window{base, base + cap * tf}, Window{base, base + cap * tf + 1},
                          Window{base, base + cap * tf + tf / 2}, Window{base + tf / 2, base + cap * tf + tf / 2},
                          Window{base + tf / 2, base + (cap - 1) * tf + tf / 2}, Window{base, base + (cap - 1) * tf}}) {
        // The real path: the backfill window, then a page of every touched bucket into
        // the buffer, which retains that window and evicts beyond kMaxBuckets.
        FlowBackfillState backfill;
        backfill.select("BTC-USD", tf);
        backfill.setWindow(w.start, w.end);
        FlowSeriesBuffer buffer;
        buffer.retainWindow(tf, backfill.windowStartMs(), backfill.windowEndMs());
        FlowSeriesBuffer::Batch batch;
        batch.symbol = "BTC-USD"; batch.timeframeMs = tf; batch.history = true;
        for (int64_t t = w.start / tf * tf; t < w.end; t += tf) {
            auto& record = batch.bars.emplace_back();
            record.bar.startMs = t; record.bar.timeframeMs = tf; record.bar.baseScale = 1; record.complete = true;
            record.source = SentinelStreamClient::FlowSource::Store;
        }
        buffer.apply(batch, 0);
        const auto* series = buffer.series(tf);
        ASSERT_NE(series, nullptr);
        int64_t retained = 0;
        for (const auto& [start, record] : series->bars) retained += start + tf > w.start && start < w.end;
        const int64_t touched = FlowSeriesBuffer::bucketsTouching(w.start, w.end, tf);
        EXPECT_EQ(retained, std::min(touched, cap)) << w.start << ' ' << w.end;
        EXPECT_EQ(touched > cap, retained < touched) << "warn exactly when buckets are dropped";
    }
}
