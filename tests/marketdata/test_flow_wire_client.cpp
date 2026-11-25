#include <gtest/gtest.h>
#include "protocol/SentinelStreamClientParseHelpers.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <future>

namespace {
using Client = SentinelStreamClient;
using Json = nlohmann::json;
using Bytes = sentinel::flow::Bytes;
sentinel::flow::FlowBar sample() {
    sentinel::flow::FlowBar b;
    b.startMs = 60'000;
    b.timeframeMs = 60'000;
    b.nativeTick = 0.01;
    b.priceScale = 100;
    b.baseScale = b.quoteScale = 1000;
    b.open = b.high = b.low = b.close = 100;
    b.baseVolume = 6; b.quoteVolume = 600; b.count = 2;
    b.buyVol = 4; b.sellVol = 2; b.delta = 2; b.maxRowVol = 6;
    b.rows = {{0, 2, 4, 1, 1}};
    b.notable = {{"trade-7", 60'123, 100, 4, 400, 100, 100, sentinel::flow::Side::Buy, 1}};
    return b;
}
Json metadata() {
    return {{"bucket_start_ms", 60'000}, {"revision", 7}, {"complete", true}, {"source", "store"}};
}
Json live() {
    auto m = metadata();
    m["source"] = "live";
    m["type"] = "flow_bar";
    m["schema_version"] = 1;
    m["sub"] = 17;
    m["symbol"] = "BTC-USD";
    m["timeframe_ms"] = 60'000;
    m["price_scale"] = 100;
    m["row_tick_units"] = 1;
    m["closed"] = true;
    m["encoding"] = "FLB1/base64";
    const auto bytes = sentinel::flow::encodeFlowBar(sample());
    m["bar"] = QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()).toBase64().toStdString();
    return m;
}
}

TEST(FlowWireClient, LiveRoundTripsCoreEncoder) {
    const auto out = protocol::clientparse::parseFlowLive(live());
    EXPECT_EQ(out.id, 17u);
    ASSERT_EQ(out.bars.size(), 1u);
    EXPECT_EQ(out.bars[0].bar, sample());
    EXPECT_EQ(out.bars[0].source, Client::FlowSource::Live);
}
TEST(FlowWireClient, RejectsCorruptionAndMetadataMismatch) {
    auto m = live(); m["bar"] = "@@@";
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
    m = live(); m["timeframe_ms"] = 100;
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
    m = live(); m["source"] = "store";
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
}
struct FlowWireClientTest : testing::Test {
    int argc = 1;
    char name[16] = "flow-wire";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    Client client{"127.0.0.1", "1"};
    void receive(const Bytes& bytes) { client.handleBinaryMessage(std::make_shared<Bytes>(bytes)); }
    void receive(const Json& message) { client.handleMessage(message.dump()); }
    void holdDecoder(std::shared_future<void> release) {
        net::post(*client.m_decodePool, [release] { release.wait(); });
    }
    void saturateDecoder() {
        client.m_decodeBacklogFrames = Client::kMaxDecodeBacklogFrames;
        client.m_decodeRefusals = 2; // an earlier heatmap backlog refusal
    }
    void drain() {
        std::promise<void> done;
        net::post(*client.m_decodePool, [&] { done.set_value(); });
        done.get_future().wait();
        QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    }
    std::vector<Json> requests() {
        client.m_ioc.restart(); client.m_ioc.poll();
        std::vector<Json> out;
        for (const auto& payload : client.m_writeQueue) out.push_back(Json::parse(payload));
        client.m_writeQueue.clear();
        return out;
    }
};
TEST_F(FlowWireClientTest, LiveReplayBarrierUsesFifoWorkerAndQueuedDelivery) {
    std::vector<Client::FlowBatchPtr> seen;
    QObject::connect(&client, &Client::flowBarsReceived, &client,
                     [&](auto b) { seen.push_back(std::move(b)); }, Qt::QueuedConnection);
    std::promise<void> release;
    holdDecoder(release.get_future().share());
    receive(live()); receive(Json{{"type","flow_live_ready"},{"sub",17},{"live_from_ms",60000},{"buckets",1}});
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    EXPECT_TRUE(seen.empty()); // neither decode may run inline on the caller
    release.set_value();
    drain();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_FALSE(seen[0]->replayReady);
    EXPECT_TRUE(seen[1]->replayReady);
    EXPECT_EQ(seen[1]->id, 17u);
    EXPECT_EQ(seen[1]->liveFromMs, 60000);
}
TEST_F(FlowWireClientTest, RequestsAndSubscribeRefusalsAreCorrelated) {
    const auto sub = client.subscribeFlowLive("BTC-USD", 1000);
    client.unsubscribeFlowLive(sub);
    const auto sent = requests();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0]["sub"], sub);
    EXPECT_EQ(sent[1]["type"], "flow_live_unsubscribe");
    quint64 refused = 0;
    QObject::connect(&client, &Client::flowLiveFailed, &client,
        [&](quint64 id, const QString&) { refused = id; });
    receive(Json{{"type","error"},{"context","flow_live_subscribe"},{"sub",sub},{"message","busy"}});
    EXPECT_EQ(refused, sub);
}
TEST_F(FlowWireClientTest, DroppedLiveFrameReportsBucketOnlyOncePerThrottleInterval) {
    std::vector<int64_t> dropped;
    QObject::connect(&client, &Client::flowLiveDropped, &client,
                     [&](quint64 sub, const QString& symbol, int64_t tf, int64_t bucket) {
                         if (sub == 17 && symbol == "BTC-USD" && tf == 60'000)
                             dropped.push_back(bucket);
                     }, Qt::QueuedConnection);
    saturateDecoder();
    receive(live()); receive(live());
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped.front(), 60'000);
}

TEST(FlowWireClient, LiveCoverageAndReplayBarrierAreValidated) {
    for (const auto status : {"complete", "partial", "building", "unrecorded"}) {
        auto j = live(); j["coverage"] = status;
        const auto out = protocol::clientparse::parseFlowLive(j);
        ASSERT_EQ(out.ranges.size(), 1u);
        EXPECT_EQ(out.ranges[0].fromMs, 60000);
        EXPECT_EQ(out.ranges[0].toMs, 120000);
    }
    auto j = live(); j["coverage"] = "invented";
    EXPECT_THROW(protocol::clientparse::parseFlowLive(j), std::exception);
    j = live(); j["revision"] = uint64_t(UINT32_MAX)+1;
    EXPECT_THROW(protocol::clientparse::parseFlowLive(j), std::exception);
    j = live(); j["bucket_start_ms"] = 0;
    EXPECT_THROW(protocol::clientparse::parseFlowLive(j), std::exception);
    j = {{"type","flow_live_ready"},{"sub",17},{"live_from_ms",-1},{"buckets",1}};
    EXPECT_THROW(protocol::clientparse::parseFlowLive(j), std::exception);
}
TEST_F(FlowWireClientTest, LostReplayBarrierReportsCorrelatedRetry) {
    quint64 failed = 0;
    QObject::connect(&client, &Client::flowLiveFailed, &client,
        [&](quint64 id, const QString&) { failed = id; }, Qt::QueuedConnection);
    saturateDecoder();
    receive(Json{{"type","flow_live_ready"},{"sub",17},{"live_from_ms",60000},{"buckets",1}});
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    EXPECT_EQ(failed, 17u);
}
