#include <gtest/gtest.h>
#include "datasources/FlowSeriesBuffer.hpp"
#include "datasources/FlowBackfillState.hpp"
#include "datasources/FlowFillTracker.hpp"
#include "datasources/FlowTimingProbe.hpp"
#include "marketdata/model/TradeData.h"
#include "chart/feeds/CvdFeed.hpp"
#include "chart/feeds/FlowFeed.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include "protocol/SentinelStreamClientParseHelpers.hpp"
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
    void flowTimingOn() { source.m_flowProbe = FlowTimingProbe(true); }
    int64_t flowRtt(quint64 id) const { return source.m_flowProbe.rttMs(id, source.m_flowClock.elapsed()); }
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
    void settle(int ms = 250) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    }
    // A history reply with one stored bar per requested bucket.
    void emitStored(const Json& request) {
        Client::FlowBatch b; b.history = true; b.symbol = QString::fromStdString(request["symbol"]);
        b.timeframeMs = request["timeframe_ms"]; b.id = request["req"];
        const int64_t end = request["end_time_ms"];
        b.oldestMs = end - (request["limit"].get<int64_t>() - 1) * b.timeframeMs;
        for (int64_t t = b.oldestMs; t <= end; t += b.timeframeMs)
            b.bars.push_back(bar(1, true, Source::Store, t, b.timeframeMs));
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
// A pan keeps coverage; only a trim reported by the series buffer forgets it.
TEST(FlowBackfillState, PanningBackReRequestsTrimmedCoverageAndCoalescesBuildingRefresh) {
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 10'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_TRUE(state.accept(reply(*first, 0), 1));
    state.setWindow(20'000, 30'000);
    const auto second = state.next(101); ASSERT_TRUE(second);
    EXPECT_TRUE(state.accept(reply(*second, 20'000), 102));
    EXPECT_TRUE(state.covers(0, 10'000)) << "panning away trims nothing";
    state.uncover(10'000, INT64_MAX);
    EXPECT_FALSE(state.covers(9000, 10'000));
    EXPECT_TRUE(state.covers(20'000, 30'000));
    state.setWindow(0, 10'000);
    const auto again = state.next(202); ASSERT_TRUE(again);
    EXPECT_EQ(again->endMs, 9000);
    auto page = reply(*again, 0);
    page.ranges = {{2000, 3000, Status::Building}, {3000, 4000, Status::Building}};
    EXPECT_TRUE(state.accept(page, 203));
    const auto refresh = state.next(30'203); ASSERT_TRUE(refresh);
    EXPECT_EQ(refresh->endMs, 3000); EXPECT_EQ(refresh->limit, 2);
}
TEST(FlowSeriesBuffer, PanningAwayKeepsBarsUnderTheCapsAndAcceptsOlderPages) {
    FlowSeriesBuffer buffer;
    buffer.setView(60'000, 60'000, 120'000, 60'000, 120'000);
    buffer.apply(batch(bar(1, true)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 1u);
    buffer.setView(60'000, 0, 60'000, 0, 60'000);
    EXPECT_TRUE(buffer.series(60'000)->bars.contains(60'000));
    EXPECT_FALSE(buffer.takeTrimmed(60'000));
    buffer.apply(batch(bar(1, true, Source::Live, 0)), 0);
    EXPECT_EQ(buffer.series(60'000)->bars.size(), 2u);
    buffer.apply(batch(bar(2, true)), 1);
    EXPECT_EQ(buffer.series(60'000)->bars.begin()->first, 0);
}

// A transport drop leaves the bucket Partial only until the stream delivers that bucket
// again; Partial reported by the source itself stays until history resolves it.
// History said Partial, then a transport drop and its recovery: the source's Partial stays.
TEST(FlowSeriesBuffer, SourcePartialSurvivesADropAndItsRecovery) {
    FlowSeriesBuffer buffer;
    auto page = batch(bar(1, false), true);
    page.ranges = {{60'000, 120'000, Status::Partial}};
    buffer.apply(page, 0);
    buffer.markDropped("BTC-USD", 60'000, 60'000);
    buffer.markDropped("BTC-USD", 60'000, 60'000); // a second drop keeps the first underlying status
    buffer.apply(batch(bar(2, false)), 1);
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Partial);
}
TEST(FlowSeriesBuffer, TransportDropClearsOnALaterLiveFrameButSourcePartialStays) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar(1, false)), 0);
    buffer.markDropped("BTC-USD", 60'000, 60'000);
    const auto status = [&] { return buffer.series(60'000)->coverage.at(60'000).status; };
    EXPECT_EQ(status(), Status::Partial);
    buffer.apply(batch(bar(3, false)), 1);
    EXPECT_EQ(status(), Status::Live) << "a later cumulative frame restores delivery";
    buffer.markDropped("BTC-USD", 60'000, 60'000);
    auto page = batch(bar(3, false), true);
    page.ranges = {{60'000, 120'000, Status::Partial}};
    buffer.apply(page, 2);
    buffer.apply(batch(bar(4, false)), 3);
    EXPECT_EQ(status(), Status::Partial) << "the source's own Partial is not a transport drop";
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
// Refused or quiet subscription (live not armed): history pages the older buckets first,
// then samples the live bucket once per refresh interval, never as paced edge pages.
TEST(FlowBackfillState, UnarmedLiveBucketIsSampledOnTheRefreshInterval) {
    constexpr int64_t tf = 60'000, live = 1000 * tf, interval = FlowSeriesBuffer::kRefetchIntervalMs;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.setWindow(live - 10 * tf, live + 2 * tf);
    state.markLiveEdge(live);
    const auto history = state.next(0); ASSERT_TRUE(history);
    EXPECT_EQ(history->endMs, live - tf) << "an unarmed edge is sampled, not paged";
    EXPECT_EQ(history->limit, 10);
    ASSERT_TRUE(state.accept(reply(*history, live - 10 * tf), 10));
    const auto sample = state.next(110); ASSERT_TRUE(sample);
    EXPECT_EQ(sample->endMs, live);
    EXPECT_EQ(sample->limit, 1);
    EXPECT_TRUE(sample->sample);
    ASSERT_TRUE(state.accept(reply(*sample, live), 120));
    EXPECT_TRUE(state.covers(live - 10 * tf, live + tf));
    EXPECT_FALSE(state.next(220));
    EXPECT_EQ(state.lastHold(), FlowBackfillState::Hold::Idle);
    EXPECT_EQ(state.wakeAtMs(220), 110 + interval) << "the pager wakes for the next sample";
    // The edge moves on: the new live bucket waits for the interval, and the bucket that
    // closed is not paged on its own.
    state.markLiveEdge(live + tf);
    EXPECT_FALSE(state.next(5000));
    EXPECT_EQ(state.lastHold(), FlowBackfillState::Hold::LiveSample);
    EXPECT_EQ(state.liveSampleDueMs(), 110 + interval);
    EXPECT_TRUE(state.workDue(5000));
    const auto again = state.next(110 + interval); ASSERT_TRUE(again);
    EXPECT_EQ(again->endMs, live + tf);
    EXPECT_EQ(again->limit, 2) << "the closed bucket is sampled once more with the live one";
}
// The live seam: a view running past the live bucket never makes history name a later
// bucket, armed or not, through pages, refreshes or seal probes.
TEST(FlowBackfillState, NothingAfterTheLiveBucketIsRequested) {
    constexpr int64_t tf = 60'000, live = 1000 * tf;
    for (const bool armed : {false, true}) {
        FlowBackfillState state; state.select("BTC-USD", tf);
        state.setWindow(live - 100 * tf, live + 50 * tf);
        state.markLiveEdge(live);
        if (armed) state.observeLive(live, 0);
        state.refresh(live + 2 * tf, 0);
        int64_t newest = -1;
        int sent = 0;
        for (int64_t now = 0; now <= 70'000; now += 50) {
            const auto request = state.next(now);
            if (!request) continue;
            ++sent;
            newest = std::max(newest, request->endMs);
            auto page = reply(*request, request->endMs - (request->limit - 1) * tf);
            page.bars = {bar(1, false, Source::Live, request->endMs, tf)}; // provisional: arms the seal probe
            ASSERT_TRUE(state.accept(page, now));
        }
        EXPECT_GT(sent, 2) << "armed=" << armed;
        EXPECT_EQ(newest, armed ? live - tf : live) << "armed=" << armed;
    }
}
// The logged stall (1h chart, live not armed): the view ran past the live bucket, the
// newest gaps were empty buckets at and after the edge, and each empty edge reply held
// the next page for 5 s while the older visible gap waited. It now goes out at once.
TEST(FlowBackfillState, EmptyEdgeReplyDoesNotDelayTheOlderVisibleGap) {
    constexpr int64_t tf = 3'600'000, live = 500'000 * tf;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.markLiveEdge(live);
    state.setWindow(live - 5 * tf, live + tf);
    const auto first = state.next(0); ASSERT_TRUE(first);
    ASSERT_TRUE(state.accept(reply(*first, live - 5 * tf), 20));
    for (int64_t now = 120; now < 1000; now += 100) {
        if (const auto edge = state.next(now)) { // the empty edge reply
            EXPECT_LE(edge->endMs, live);
            ASSERT_TRUE(state.accept(reply(*edge, edge->endMs), now + 20));
        }
    }
    // Pan to past the live bucket; older visible buckets are not loaded.
    state.setWindow(live - 20 * tf, live + 4 * tf);
    state.markLiveEdge(live);
    std::optional<FlowBackfillState::Request> older;
    for (int64_t now = 1000; now < 1300 && !older; now += 100) older = state.next(now);
    ASSERT_TRUE(older) << "held: " << FlowBackfillState::holdName(state.lastHold());
    EXPECT_EQ(older->endMs, live - 6 * tf);
    EXPECT_EQ(older->limit, 15);
}
// Live armed: the stream owns the live bucket. Pages and seal probes stop before it; a
// refresh of it (a reported loss) waits until the seam passes it.
TEST(FlowBackfillState, ArmedLiveBucketIsNeverRequestedByHistory) {
    constexpr int64_t tf = 60'000, live = 1000 * tf;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.setWindow(live - 10 * tf, live + 3 * tf);
    state.markLiveEdge(live);
    state.observeLive(live, 0);
    state.refresh(live, 0);
    std::vector<int64_t> ends;
    for (int64_t now = 0; now <= 61'000; now += 100) {
        if (const auto request = state.next(now)) {
            ends.push_back(request->endMs);
            auto page = reply(*request, request->endMs - (request->limit - 1) * tf);
            page.bars = {bar(1, false, Source::Live, request->endMs, tf)};
            ASSERT_TRUE(state.accept(page, now + 10));
        }
    }
    ASSERT_GE(ends.size(), 3u) << "one page and two seal probes";
    for (const auto end : ends) EXPECT_LT(end, live);
    EXPECT_FALSE(state.workDue(61'000));
    state.markLiveEdge(live + tf);
    const auto closed = state.next(61'200); ASSERT_TRUE(closed);
    EXPECT_EQ(closed->endMs, live) << "once closed, the bucket is history's";
    EXPECT_TRUE(closed->refresh);
}
// A live sample that failed and waits for its retry: live arms meanwhile. The retry drops
// the bucket that is now the stream's and keeps the closed bucket it also covered.
TEST(FlowBackfillState, HeldSampleDropsItsNowLiveBucketWhenLiveArms) {
    constexpr int64_t tf = 60'000, live = 1000 * tf, interval = FlowSeriesBuffer::kRefetchIntervalMs;
    for (const bool withClosedBucket : {true, false}) {
        FlowBackfillState state; state.select("BTC-USD", tf);
        state.setWindow(live - 10 * tf, live + 2 * tf);
        state.markLiveEdge(live);
        const auto history = state.next(0); ASSERT_TRUE(history);
        ASSERT_TRUE(state.accept(reply(*history, live - 10 * tf), 10));
        int64_t now = 110, edge = live;
        if (withClosedBucket) {
            const auto first = state.next(now); ASSERT_TRUE(first); ASSERT_TRUE(first->sample);
            ASSERT_TRUE(state.accept(reply(*first, live), now + 10));
            edge = live + tf;
            state.markLiveEdge(edge);
            now += interval;
        }
        const auto sample = state.next(now); ASSERT_TRUE(sample);
        ASSERT_TRUE(sample->sample);
        ASSERT_EQ(sample->endMs, edge);
        ASSERT_EQ(sample->limit, withClosedBucket ? 2 : 1);
        ASSERT_TRUE(state.fail(sample->id, Client::FlowFailure::Retryable, now + 10));
        state.observeLive(edge, now + 20);
        const auto retry = state.next(now + 2000);
        if (!withClosedBucket) {
            EXPECT_FALSE(retry) << "only the now-live bucket was held: nothing to resend";
            continue;
        }
        ASSERT_TRUE(retry);
        EXPECT_EQ(retry->endMs, live) << "the closed bucket only";
        EXPECT_EQ(retry->limit, 1);
        EXPECT_FALSE(retry->sample);
    }
}
// A refresh held at the armed seam names the bucket boundary to wake at, so it goes out
// when the bucket closes even if no live frame or view change calls the pager.
TEST(FlowBackfillState, RefreshHeldAtTheSeamWakesAtTheBucketBoundary) {
    constexpr int64_t tf = 60'000, live = 1000 * tf;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.setWindow(live - 10 * tf, live + tf);
    state.markLiveEdge(live);
    state.observeLive(live, 0);
    EXPECT_EQ(state.seamWakeWallMs(), 0) << "nothing waits on the seam";
    const auto page = state.next(0); ASSERT_TRUE(page);
    ASSERT_TRUE(state.accept(reply(*page, live - 10 * tf), 10));
    state.refresh(live, 20);
    EXPECT_FALSE(state.next(200));
    EXPECT_EQ(state.wakeAtMs(200), FlowSeriesBuffer::kRefetchIntervalMs) << "only the seal probe's deadline";
    EXPECT_EQ(state.seamWakeWallMs(), live + tf);
    state.markLiveEdge(live + tf);
    EXPECT_EQ(state.seamWakeWallMs(), 0);
    const auto closed = state.next(300); ASSERT_TRUE(closed);
    EXPECT_EQ(closed->endMs, live);
    EXPECT_TRUE(closed->refresh);
}
// The wake is the close of the earliest deferred bucket, not every boundary before it.
TEST(FlowBackfillState, SeamWakeIsTheCloseOfTheEarliestDeferredBucket) {
    constexpr int64_t tf = 60'000, live = 1000 * tf;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.setWindow(live - 10 * tf, live + tf);
    state.markLiveEdge(live);
    state.observeLive(live, 0);
    state.refresh(live + 3 * tf, 0);
    EXPECT_EQ(state.seamWakeWallMs(), live + 4 * tf);
    state.refresh(live, 0);
    EXPECT_EQ(state.seamWakeWallMs(), live + tf);
}
// A live sample that failed waits for its retry; the view pans to older, unloaded history
// meantime. The older gap goes first; the sample is taken again after it.
TEST(FlowBackfillState, HeldSampleWaitsBehindOlderGapsAfterAPan) {
    constexpr int64_t tf = 60'000, live = 1000 * tf;
    FlowBackfillState state; state.select("BTC-USD", tf);
    state.setWindow(live - 10 * tf, live + tf);
    state.markLiveEdge(live);
    const auto history = state.next(0); ASSERT_TRUE(history);
    ASSERT_TRUE(state.accept(reply(*history, live - 10 * tf), 10));
    const auto sample = state.next(110); ASSERT_TRUE(sample); ASSERT_TRUE(sample->sample);
    ASSERT_TRUE(state.fail(sample->id, Client::FlowFailure::Retryable, 120));
    state.setWindow(live - 30 * tf, live + tf);
    const auto older = state.next(1200); ASSERT_TRUE(older);
    EXPECT_FALSE(older->sample) << "the retry waited behind the older gap";
    EXPECT_EQ(older->endMs, live - 11 * tf);
    EXPECT_EQ(older->limit, 20);
    ASSERT_TRUE(state.accept(reply(*older, live - 30 * tf), 1210));
    const auto again = state.next(1400); ASSERT_TRUE(again);
    EXPECT_TRUE(again->sample);
    EXPECT_EQ(again->endMs, live);
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
// Coverage outside the window lasts until the series buffer trims those bars: returning
// to a live interval pages only the minutes never loaded, and a reported trim again.
TEST(FlowBackfillState, ReturningToALiveIntervalPagesOnlyWhatWasNotKept) {
    constexpr int64_t hour = 3'600'000, minute = 60'000;
    FlowBackfillState state; state.select("BTC-USD", minute);
    state.setWindow(9 * hour + 40 * minute, 10 * hour + 31 * minute);
    state.beginLive(10 * hour);
    for (int m = 0; m <= 30; ++m) state.markLiveEdge(10 * hour + m * minute);
    state.setWindow(7 * hour, 9 * hour);
    state.markLiveEdge(10 * hour + 40 * minute);
    state.setWindow(9 * hour + 40 * minute, 10 * hour + 41 * minute);
    state.markLiveEdge(10 * hour + 40 * minute);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(first->endMs, 10 * hour + 39 * minute);
    EXPECT_EQ(first->limit, 9) << "the minutes the edge passed while the view was away";
    ASSERT_TRUE(state.accept(reply(*first, 10 * hour + 31 * minute), 1));
    const auto older = state.next(101); ASSERT_TRUE(older);
    EXPECT_EQ(older->endMs, 9 * hour + 59 * minute) << "the live interval stayed loaded";
    ASSERT_TRUE(state.accept(reply(*older, 9 * hour + 40 * minute), 102));
    EXPECT_FALSE(state.next(202));
    state.uncover(10 * hour + 20 * minute, INT64_MAX);
    const auto trimmed = state.next(302); ASSERT_TRUE(trimmed);
    EXPECT_EQ(trimmed->endMs, 10 * hour + 19 * minute);
    EXPECT_EQ(trimmed->limit, 40);
}
// A silent armed subscription: the clock alone advances the covered edge, and a view
// running past it sends history nothing.
TEST(FlowBackfillState, SilentSubscriptionCoversAdvancingEdge) {
    FlowBackfillState state; state.select("BTC-USD", 1000);
    state.setWindow(60'000, 61'000);
    state.beginLive(60'000);
    int requests = 0;
    for (int second = 0; second < 60; ++second) {
        state.setWindow(60'000, 61'000 + second * 1000);
        if (second % 5 == 0) state.markLiveEdge(60'000 + second * 1000);
        if (auto req = state.next(second * 1000)) {
            ++requests;
            EXPECT_TRUE(state.accept(reply(*req, req->endMs), second * 1000 + 1));
        }
    }
    EXPECT_EQ(requests, 0);
    EXPECT_TRUE(state.covers(60'000, 116'000));
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
        EXPECT_EQ(probe->endMs, (9 + i) * 60'000) << "the live bucket is the stream's";
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
// A dropped frame on the forming bucket (1 s bars, live armed): no history request for it
// while it is live, a later frame restores it, and once it closes its refresh goes out by
// the pager's own boundary wake, with no further frame or view change.
TEST_F(FlowDataSourceTest, DroppedLiveFrameRecoversAndRefreshesTheBucketAfterItCloses) {
    constexpr int64_t tf = 1000;
    QElapsedTimer align; align.start();
    while (QDateTime::currentMSecsSinceEpoch() % tf > 100 && align.elapsed() < 2000) {}
    const int64_t live = QDateTime::currentMSecsSinceEpoch() / tf * tf;
    ready();
    source.subscribeFlowLive("BTC-USD", tf);
    const auto subscribe = requests(); ASSERT_EQ(subscribe.size(), 1u);
    const auto sub = subscribe[0]["sub"].get<quint64>();
    const auto frame = [&](uint32_t revision) {
        auto b = batch(bar(revision, false, Source::Live, live, tf)); b.id = sub;
        client().flowBarsReceived(std::make_shared<const Client::FlowBatch>(b)); deliver();
    };
    frame(1);
    source.requestFlowHistory("BTC-USD", tf, live - 10 * tf, live + tf);
    const auto page = requests(); ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0]["end_time_ms"], live - tf);
    emitPage(page[0]);
    client().flowLiveDropped(sub, "BTC-USD", tf, live); deliver();
    const auto status = [&] { return source.flowBuffer().series(tf)->coverage.at(live).status; };
    EXPECT_EQ(status(), Status::Partial);
    settle(150);
    if (QDateTime::currentMSecsSinceEpoch() / tf * tf != live) GTEST_SKIP() << "crossed a second boundary";
    EXPECT_TRUE(requests().empty()) << "the live bucket is the stream's";
    frame(2);
    EXPECT_EQ(status(), Status::Live) << "a later frame restores delivery";
    QElapsedTimer clock; clock.start();
    std::vector<Json> refetch;
    while (refetch.empty() && clock.elapsed() < 2500) {
        settle(50);
        refetch = requests();
    }
    ASSERT_EQ(refetch.size(), 1u) << "the pager wakes when the bucket closes";
    EXPECT_GE(QDateTime::currentMSecsSinceEpoch(), live + tf);
    EXPECT_EQ(refetch[0]["end_time_ms"], live);
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
// The server never acknowledges a flow subscription; a refusal is an error frame and
// nothing more. Until the subscription delivers a live frame, the live edge is not
// covered, so history pages it (a refused subscription never fills it otherwise).
TEST_F(FlowDataSourceTest, RefusedSubscriptionLeavesTheLiveEdgeToHistory) {
    constexpr int64_t tf = 60'000;
    ready();
    const int64_t live = QDateTime::currentMSecsSinceEpoch() / tf * tf;
    source.subscribeFlowLive("BTC-USD", tf);
    const auto subscribe = requests(); ASSERT_EQ(subscribe.size(), 1u);
    receiveWire({{"type", "error"}, {"context", "flow_live_subscribe"}, {"sub", subscribe[0]["sub"]},
                 {"symbol", "BTC-USD"}, {"message", "unsupported flow series or capacity reached"}});
    deliver();
    source.requestFlowHistory("BTC-USD", tf, live - 10 * tf, live + tf);
    if (QDateTime::currentMSecsSinceEpoch() / tf * tf != live) GTEST_SKIP() << "crossed a minute boundary";
    const auto page = requests(); ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0]["type"], "flow_history_request");
    EXPECT_EQ(page[0]["end_time_ms"], live - tf) << "older buckets first";
    emitPage(page[0]);
    settle();
    if (QDateTime::currentMSecsSinceEpoch() / tf * tf != live) GTEST_SKIP() << "crossed a minute boundary";
    const auto sample = requests(); ASSERT_EQ(sample.size(), 1u);
    EXPECT_EQ(sample[0]["end_time_ms"], live) << "the undelivered live bucket is sampled";
    EXPECT_EQ(sample[0]["limit"], 1);
    emitPage(sample[0]);
    settle();
    EXPECT_TRUE(requests().empty());
    EXPECT_GT(flowDelay(), FlowSeriesBuffer::kRefetchIntervalMs - 1000) << "next sample after the refresh interval";
    EXPECT_LE(flowDelay(), FlowSeriesBuffer::kRefetchIntervalMs);
}
// A symbol switch with the view running past the live edge (follow live): the first page
// is the history below the live bucket, never the empty buckets after it.
TEST_F(FlowDataSourceTest, SwitchPagesHistoryBelowTheLiveEdgeInTheFirstPage) {
    constexpr int64_t tf = 60'000;
    ready();
    const int64_t live = QDateTime::currentMSecsSinceEpoch() / tf * tf;
    source.subscribeFlowLive("ETH-USD", tf);
    source.requestFlowHistory("ETH-USD", tf, live - 30 * tf, live + 3 * tf);
    if (QDateTime::currentMSecsSinceEpoch() / tf * tf != live) GTEST_SKIP() << "crossed a minute boundary";
    const auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1]["end_time_ms"], live - tf);
    EXPECT_EQ(sent[1]["limit"], 63) << "one page reaches the window start (one-view margin included)";
}
// Panning keeps what was loaded: a pan right then back left by one screen sends no page
// for the buckets that stayed in the padding.
TEST_F(FlowDataSourceTest, PanRightThenLeftKeepsPaddingWithoutRefetch) {
    constexpr int64_t tf = 60'000, screen = 60 * tf, start = 1000 * tf;
    ready();
    source.subscribeFlowLive("BTC-USD", tf);
    source.requestFlowHistory("BTC-USD", tf, start, start + screen);
    auto sent = requests(); ASSERT_EQ(sent.size(), 2u);
    ASSERT_EQ(sent[1]["end_time_ms"], start + screen - tf);
    ASSERT_EQ(sent[1]["limit"], 120) << "the view and its one-screen margin";
    emitStored(sent[1]);
    settle();
    source.requestFlowHistory("BTC-USD", tf, start + screen, start + 2 * screen);
    settle();
    sent = requests(); ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0]["end_time_ms"], start + 2 * screen - tf);
    EXPECT_EQ(sent[0]["limit"], 60);
    emitStored(sent[0]);
    settle();
    source.requestFlowHistory("BTC-USD", tf, start, start + screen);
    settle();
    EXPECT_TRUE(requests().empty()) << "the margin left of the first view stayed loaded";
    const auto* series = source.flowBuffer().series(tf); ASSERT_NE(series, nullptr);
    EXPECT_EQ(series->bars.size(), 180u);
    EXPECT_EQ(series->bars.begin()->first, start - screen);
}
TEST_F(FlowDataSourceTest, DeliveredLiveFrameCoversTheLiveEdge) {
    constexpr int64_t tf = 60'000;
    ready();
    const int64_t live = QDateTime::currentMSecsSinceEpoch() / tf * tf;
    source.subscribeFlowLive("BTC-USD", tf);
    const auto subscribe = requests(); ASSERT_EQ(subscribe.size(), 1u);
    auto frame = batch(bar(1, false, Source::Live, live, tf));
    frame.id = subscribe[0]["sub"].get<quint64>();
    client().flowBarsReceived(std::make_shared<const Client::FlowBatch>(frame)); deliver();
    source.requestFlowHistory("BTC-USD", tf, live - 10 * tf, live + tf);
    if (QDateTime::currentMSecsSinceEpoch() / tf * tf != live) GTEST_SKIP() << "crossed a minute boundary";
    const auto page = requests(); ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0]["end_time_ms"], live - tf) << "live delivery covers its own bucket";
    emitPage(page[0]);
    settle();
    EXPECT_TRUE(requests().empty()) << "and history never asks for it";
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
    // Off pages exactly the original one-span margin; the loaded history stays under the caps.
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour,
        chart::cvdHistoryAnchor(12 * hour, tf, 0, chart::CvdReset::UtcDay, 4));
    EXPECT_EQ(flowStart(), 9 * hour); EXPECT_EQ(source.flowBuffer().series(tf)->bars.begin()->first, 0);
    source.requestFlowHistory("BTC-USD", tf, 12 * hour, 15 * hour,
        chart::cvdHistoryAnchor(12 * hour, tf, 96, chart::CvdReset::Visible, 4));
    EXPECT_EQ(flowStart(), 9 * hour); EXPECT_EQ(source.flowBuffer().series(tf)->bars.begin()->first, 0);
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
// A trim that cuts through a coverage range keeps the proof of the part it keeps.
TEST(FlowSeriesBuffer, RetentionKeepsCoverageCrossingTheCut) {
    FlowSeriesBuffer buffer(0);
    buffer.setView(60000, 3600000, 7200000, 1800000, 7200000);
    buffer.setCoverage("BTC-USD", 60000, {0, 3600000, Status::Complete});
    buffer.apply(batch(bar(1, true, Source::Store, 1740000)), 0);
    const auto* series = buffer.series(60000);
    ASSERT_NE(series, nullptr); ASSERT_EQ(series->coverage.size(), 1u);
    EXPECT_TRUE(series->bars.empty());
    EXPECT_EQ(series->coverage.begin()->second.fromMs, 1800000);
    EXPECT_EQ(series->coverage.begin()->second.toMs, 3600000);
    const auto trim = buffer.takeTrimmed(60000); ASSERT_TRUE(trim);
    EXPECT_EQ(trim->belowMs, 1800000);
    EXPECT_EQ(trim->fromMs, INT64_MAX);
}
namespace {
Client::FlowRecord sizedBar(int64_t start, int64_t tf, size_t rows = 100) {
    auto record = bar(1, true, Source::Store, start, tf);
    record.bar.rows.resize(rows);
    record.bar.rows.shrink_to_fit();
    return record;
}
Client::FlowBatch storedPage(int64_t tf, int64_t fromBucket, int64_t toBucket) {
    Client::FlowBatch b; b.symbol = "BTC-USD"; b.timeframeMs = tf; b.history = true;
    for (int64_t i = fromBucket; i < toBucket; ++i) b.bars.push_back(sizedBar(i * tf, tf));
    return b;
}
size_t bytesOutside(const FlowSeriesBuffer::Series& series, int64_t keepFrom, int64_t keepTo) {
    size_t bytes = 0;
    for (const auto& [start, record] : series.bars)
        if (start < keepFrom || start >= keepTo) bytes += FlowSeriesBuffer::approxBarBytes(record);
    return bytes;
}
}
// Bytes outside the kept span are capped; the trim takes the side farther from the view.
TEST(FlowSeriesBuffer, ByteCapTrimsTheFarSideFirst) {
    constexpr int64_t tf = 60'000;
    const size_t each = FlowSeriesBuffer::approxBarBytes(sizedBar(0, tf));
    FlowSeriesBuffer buffer(40 * each);
    buffer.setView(tf, 100 * tf, 110 * tf, 90 * tf, 110 * tf); // padding [80, 130)
    buffer.apply(storedPage(tf, 0, 130), 0); // 90 bars before the kept span, 20 after it
    const auto* series = buffer.series(tf); ASSERT_NE(series, nullptr);
    EXPECT_LE(bytesOutside(*series, 90 * tf, 110 * tf), 40 * each);
    EXPECT_EQ(series->bars.size(), 60u);
    EXPECT_EQ(series->bars.begin()->first, 70 * tf) << "the 70 farthest bars were all older";
    EXPECT_EQ(series->bars.rbegin()->first, 129 * tf) << "the nearer side kept everything";
    const auto trim = buffer.takeTrimmed(tf); ASSERT_TRUE(trim);
    EXPECT_EQ(trim->belowMs, 70 * tf);
    EXPECT_EQ(trim->fromMs, INT64_MAX);
    EXPECT_FALSE(buffer.takeTrimmed(tf)) << "a trim is reported once";
    buffer.setView(tf, 101 * tf, 111 * tf, 91 * tf, 111 * tf);
    EXPECT_FALSE(buffer.takeTrimmed(tf)) << "a pan under the cap trims nothing";
}
TEST(FlowSeriesBuffer, PaddingIsTwoScreensEachSideAndFourTowardThePan) {
    constexpr int64_t tf = 60'000;
    FlowSeriesBuffer buffer;
    buffer.setView(tf, 100 * tf + 30'000, 110 * tf + 30'000, 90 * tf, 111 * tf);
    auto pad = buffer.padding(tf); ASSERT_TRUE(pad);
    EXPECT_EQ(pad->fromMs, 80 * tf) << "rounded out to whole buckets";
    EXPECT_EQ(pad->toMs, 131 * tf);
    EXPECT_EQ(pad->panDirection, 0);
    buffer.setView(tf, 110 * tf, 120 * tf, 100 * tf, 120 * tf); // pan right
    pad = buffer.padding(tf);
    EXPECT_EQ(pad->panDirection, 1); EXPECT_EQ(pad->fromMs, 90 * tf); EXPECT_EQ(pad->toMs, 160 * tf);
    buffer.setView(tf, 105 * tf, 125 * tf, 85 * tf, 125 * tf); // zoom out about the same centre
    pad = buffer.padding(tf);
    EXPECT_EQ(pad->panDirection, 1); EXPECT_EQ(pad->fromMs, 65 * tf); EXPECT_EQ(pad->toMs, 205 * tf);
    buffer.setView(tf, 95 * tf, 115 * tf, 75 * tf, 115 * tf); // pan left
    pad = buffer.padding(tf);
    EXPECT_EQ(pad->panDirection, -1); EXPECT_EQ(pad->fromMs, 15 * tf); EXPECT_EQ(pad->toMs, 155 * tf);
    // After a pan right the newer side has twice the padding, so it keeps more under the cap.
    const size_t each = FlowSeriesBuffer::approxBarBytes(sizedBar(0, tf));
    FlowSeriesBuffer capped(40 * each);
    capped.setView(tf, 100 * tf, 110 * tf, 90 * tf, 110 * tf);
    capped.setView(tf, 110 * tf, 120 * tf, 100 * tf, 120 * tf);
    capped.apply(storedPage(tf, 60, 160), 0); // 40 bars outside on each side
    const auto* series = capped.series(tf); ASSERT_NE(series, nullptr);
    EXPECT_LE(bytesOutside(*series, 100 * tf, 120 * tf), 40 * each);
    const auto older = std::distance(series->bars.begin(), series->bars.lower_bound(100 * tf));
    const auto newer = std::distance(series->bars.lower_bound(120 * tf), series->bars.end());
    EXPECT_EQ(older + newer, 40);
    EXPECT_GT(newer, 3 * older) << "older=" << older << " newer=" << newer;
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
        buffer.setView(tf, w.start, w.end, backfill.windowStartMs(), backfill.windowEndMs());
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

TEST(FlowSeriesBuffer, UnrecordedCoversEmptyWindowWithoutRetryWhileOldBuildingStillRetries) {
    for (const auto status : {Status::Unrecorded, Status::Building}) {
        FlowBackfillState state;
        FlowSeriesBuffer buffer;
        state.select("BTC-USD", 60'000); state.setWindow(0, 120'000);
        const auto request = state.next(0);
        ASSERT_TRUE(request);
        const auto wireStatus = status == Status::Unrecorded ? "unrecorded" : "building";
        const nlohmann::json header = {{"type", "flow_history_chunk"}, {"schema_version", 1},
            {"req", request->id}, {"symbol", "BTC-USD"}, {"timeframe_ms", 60'000},
            {"oldest_ms", 0}, {"sealed_through_ms", 0}, {"status", wireStatus},
            {"ranges", {{{"from_ms", 0}, {"to_ms", 60'000}, {"status", wireStatus}},
                        {{"from_ms", 60'000}, {"to_ms", 120'000}, {"status", wireStatus}}}},
            {"bars", nlohmann::json::array()}};
        const auto json = header.dump();
        sentinel::flow::Bytes frame{'F', 'H', 'E', '1'};
        for (unsigned i = 0; i < 4; ++i) frame.push_back(uint8_t(json.size() >> (8 * i)));
        frame.insert(frame.end(), json.begin(), json.end());
        const auto page = protocol::clientparse::parseFlowHistory(frame);
        ASSERT_TRUE(state.accept(page, 10));
        EXPECT_TRUE(buffer.apply(page, 10).empty());
        const auto* series = buffer.series(60'000);
        ASSERT_NE(series, nullptr);
        EXPECT_TRUE(series->bars.empty());
        ASSERT_EQ(series->coverage.size(), 2u);
        EXPECT_EQ(series->coverage.at(0).status, status);
        EXPECT_FALSE(state.next(1000));
        const auto retry = state.next(FlowSeriesBuffer::kRefetchIntervalMs + 10);
        EXPECT_EQ(retry.has_value(), status == Status::Building);
    }
}

TEST(FlowSeriesBuffer, UnrecordedWithdrawsOldGenerationAndAllowsNewRecordingRevisionOne) {
    FlowSeriesBuffer buffer;
    buffer.apply(batch(bar(9, true, Source::Store)), 0);
    auto absent = batch(bar(), true);
    absent.bars.clear(); absent.status = Status::Unrecorded;
    absent.ranges = {{60'000, 120'000, Status::Unrecorded}};
    buffer.apply(absent, 1);
    ASSERT_TRUE(buffer.series(60'000)->bars.empty());
    auto discovered = batch(bar(1, true, Source::Store), true);
    discovered.ranges = {{60'000, 120'000, Status::Complete}};
    buffer.apply(discovered, 2);
    EXPECT_EQ(buffer.series(60'000)->bars.at(60'000).revision, 1u);
    EXPECT_EQ(buffer.series(60'000)->coverage.at(60'000).status, Status::Complete);
}

// flow.timing: each hold of a due page is named; covers() answers for the fill probe.
TEST(FlowBackfillState, HoldNamesWhyADuePageWasNotSent) {
    using Hold = FlowBackfillState::Hold;
    FlowBackfillState state; state.select("BTC-USD", 1000); state.setWindow(0, 600'000);
    const auto first = state.next(0); ASSERT_TRUE(first);
    EXPECT_EQ(state.lastHold(), Hold::Sent);
    EXPECT_FALSE(state.next(1));
    EXPECT_EQ(state.lastHold(), Hold::InFlight);
    EXPECT_TRUE(state.workDue(1));
    EXPECT_FALSE(state.covers(88'000, 600'000));
    EXPECT_TRUE(state.accept(reply(*first, 88'000), 2));
    EXPECT_TRUE(state.covers(88'000, 600'000));
    EXPECT_FALSE(state.covers(87'000, 600'000));
    EXPECT_FALSE(state.next(50));
    EXPECT_EQ(state.lastHold(), Hold::Pacing);
    EXPECT_TRUE(state.workDue(50));
    const auto second = state.next(102); ASSERT_TRUE(second);
    EXPECT_TRUE(state.fail(second->id, Client::FlowFailure::Retryable, 103));
    EXPECT_FALSE(state.next(500));
    EXPECT_EQ(state.lastHold(), Hold::Backoff);
    const auto retry = state.next(1103); ASSERT_TRUE(retry);
    EXPECT_TRUE(state.accept(reply(*retry, 0), 1104));
    EXPECT_FALSE(state.next(1300));
    EXPECT_EQ(state.lastHold(), Hold::Idle);
    EXPECT_FALSE(state.workDue(1300));
    EXPECT_TRUE(state.covers(0, 600'000));
    EXPECT_STREQ(FlowBackfillState::holdName(Hold::LiveSample), "live_sample_interval");
}
TEST(FlowFillTracker, ReportsOneFillPerUncoveredWaitWithPagesAndBothDelays) {
    FlowFillTracker tracker;
    EXPECT_FALSE(tracker.viewChanged(0, 100, true, 10));
    EXPECT_FALSE(tracker.waiting()) << "a covered view starts no wait";
    EXPECT_FALSE(tracker.viewChanged(0, 100, false, 20));
    EXPECT_FALSE(tracker.waiting()) << "an unchanged range is ignored";
    EXPECT_FALSE(tracker.viewChanged(-50, 50, false, 30));
    EXPECT_TRUE(tracker.waiting());
    EXPECT_FALSE(tracker.viewChanged(-60, 40, false, 40));
    EXPECT_FALSE(tracker.pageApplied(false, 100));
    const auto filled = tracker.pageApplied(true, 250); ASSERT_TRUE(filled);
    EXPECT_EQ(filled->sinceFirstChangeMs, 220);
    EXPECT_EQ(filled->sinceLastChangeMs, 210);
    EXPECT_EQ(filled->pages, 2);
    EXPECT_EQ(filled->changes, 2);
    EXPECT_FALSE(tracker.pageApplied(true, 300)) << "a fill reports once";
    EXPECT_FALSE(tracker.viewChanged(-70, 30, false, 400));
    const auto back = tracker.viewChanged(0, 100, true, 450); ASSERT_TRUE(back);
    EXPECT_EQ(back->sinceLastChangeMs, 0);
    EXPECT_EQ(back->sinceFirstChangeMs, 50);
    EXPECT_EQ(back->pages, 0);
    EXPECT_EQ(back->changes, 2);
}
TEST(FlowTimingProbe, FillRangeIsVisibleBucketsClippedToWindowAndLiveBucket) {
    using Range = FlowTimingProbe::Range;
    const auto same = [](Range a, Range b) { return a.from == b.from && a.to == b.to; };
    EXPECT_TRUE(same(FlowTimingProbe::fillRange(90'500, 150'000, 60'000, 0, 1'000'000, 1'000'000), {60'000, 180'000}));
    EXPECT_TRUE(same(FlowTimingProbe::fillRange(90'500, 150'000, 60'000, 0, 1'000'000, 100'000), {60'000, 120'000}));
    EXPECT_TRUE(same(FlowTimingProbe::fillRange(90'500, 150'000, 60'000, 120'000, 1'000'000, 1'000'000), {120'000, 180'000}));
    EXPECT_TRUE(same(FlowTimingProbe::fillRange(90'500, 150'000, 60'000, 0, 60'000, 1'000'000), {}));
    EXPECT_TRUE(same(FlowTimingProbe::fillRange(150'000, 90'500, 60'000, 0, 1'000'000, 1'000'000), {}));
}
TEST(FlowTimingProbe, BookBytesFollowReplicaSlots) {
    LiveOrderBook empty("BTC-USD");
    EXPECT_EQ(approxBookBytes(empty), 0u);
    LiveOrderBook book("BTC-USD");
    book.initialize(0, 127, 1); // 128 slots per side
    const size_t dense = 2 * 128 * sizeof(double);
    EXPECT_GE(approxBookBytes(book), dense + 2 * 2 * (sizeof(uint64_t) + sizeof(double) + 1));
    EXPECT_LT(approxBookBytes(book), dense + 512);
}
TEST(FlowSeriesBuffer, ApproxMemoryCountsBarsRowsNotablesAndCoverage) {
    FlowSeriesBuffer buffer;
    const auto none = buffer.approxMemory();
    EXPECT_EQ(none.bars, 0u);
    EXPECT_EQ(none.totalBytes(), 0u);
    auto record = bar();
    record.bar.rows.resize(3);
    record.bar.rows.shrink_to_fit();
    record.bar.notable.resize(1);
    buffer.apply(batch(record), 0);
    const auto memory = buffer.approxMemory();
    EXPECT_EQ(memory.series, 1u);
    EXPECT_EQ(memory.bars, 1u);
    EXPECT_EQ(memory.rows, 3u);
    EXPECT_GE(memory.rowBytes, 3 * sizeof(sentinel::flow::Row));
    EXPECT_GE(memory.notableBytes, sizeof(sentinel::flow::Notable));
    EXPECT_GE(memory.nodeBytes, sizeof(FlowSeriesBuffer::Record) + FlowSeriesBuffer::kMapNodeOverheadBytes);
    EXPECT_EQ(memory.coverageRanges, 1u);
    EXPECT_EQ(memory.coveredFromMs, 60'000);
    EXPECT_EQ(memory.coveredToMs, 120'000);
    EXPECT_EQ(memory.totalBytes(), memory.rowBytes + memory.notableBytes + memory.nodeBytes);
    buffer.clear();
    EXPECT_EQ(buffer.approxMemory().totalBytes(), 0u);
}

TEST(FlowFillTracker, RecheckSettlesAWaitWithoutCountingAPage) {
    FlowFillTracker tracker;
    EXPECT_FALSE(tracker.recheck(true, 5)) << "nothing waits";
    EXPECT_FALSE(tracker.viewChanged(0, 100, false, 10));
    EXPECT_FALSE(tracker.recheck(false, 20));
    const auto filled = tracker.recheck(true, 30); ASSERT_TRUE(filled);
    EXPECT_EQ(filled->pages, 0);
    EXPECT_EQ(filled->sinceFirstChangeMs, 20);
    EXPECT_FALSE(tracker.waiting());
}
// The probe learns a new selection before its first page is sent, so that page's
// round trip is not lost to the selection reset.
TEST_F(FlowDataSourceTest, TimingProbeKeepsTheFirstPageOfANewSelection) {
    flowTimingOn();
    ready();
    select();
    quint64 id = 0;
    for (const auto& request : requests())
        if (request["type"] == "flow_history_request") id = request["req"];
    ASSERT_NE(id, 0u);
    EXPECT_GE(flowRtt(id), 0);
}
// flow.timing reports a build when its published content changes, even when
// every count stays the same.
TEST(FeedTiming, CvdSignatureFollowsFooterDeltaAtTheSameVolume) {
    auto first = std::make_unique<chart::CvdSnapshot>();
    first->series.count = 1;
    first->series.points[0] = {60'000, 60'000, 0.0, 60'000}; // visible reset: the first point stays zero
    first->totals[0] = {2.0, 10.0, true};
    auto same = std::make_unique<chart::CvdSnapshot>(*first);
    EXPECT_EQ(chart::CvdFeed::contentSignature(*first), chart::CvdFeed::contentSignature(*same));
    auto flipped = std::make_unique<chart::CvdSnapshot>(*first);
    flipped->totals[0].delta = -2.0; // buy/sell mix changed, volume did not
    EXPECT_NE(chart::CvdFeed::contentSignature(*first), chart::CvdFeed::contentSignature(*flipped));
}
TEST(FeedTiming, FootprintSignatureFollowsACellThatMovesAtTheSameVolume) {
    auto first = std::make_unique<chart::FlowSnapshot>();
    first->barCount = 1;
    first->cellCount = 1;
    first->bars[0].startMs = 60'000;
    first->bars[0].cellCount = 1;
    first->cells[0] = {100, 1, 5, 7};
    auto same = std::make_unique<chart::FlowSnapshot>(*first);
    EXPECT_EQ(chart::FlowFeed::contentSignature(*first), chart::FlowFeed::contentSignature(*same));
    auto moved = std::make_unique<chart::FlowSnapshot>(*first);
    moved->cells[0].row = 101;
    EXPECT_NE(chart::FlowFeed::contentSignature(*first), chart::FlowFeed::contentSignature(*moved));
    auto widened = std::make_unique<chart::FlowSnapshot>(*first);
    widened->cells[0].span = 2;
    EXPECT_NE(chart::FlowFeed::contentSignature(*first), chart::FlowFeed::contentSignature(*widened));
}
