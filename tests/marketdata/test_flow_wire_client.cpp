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
Json header() {
    return {{"type", "flow_history_chunk"}, {"schema_version", 1}, {"req", 91}, {"symbol", "BTC-USD"},
        {"timeframe_ms", 60'000}, {"price_scale", 100}, {"row_tick_units", 1}, {"oldest_ms", 0},
        {"sealed_through_ms", 120'000}, {"status", "building"},
        {"ranges", {{{"from_ms", 0}, {"to_ms", 60'000}, {"status", "building"}, {"price_scale", 100}, {"row_tick_units", 1}},
                    {{"from_ms", 60'000}, {"to_ms", 120'000}, {"status", "complete"}, {"price_scale", 100}, {"row_tick_units", 1}}}},
        {"bars", {metadata()}}};
}
void length(Bytes& out, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(uint8_t(n >> (8 * i)));
}
// FHE1 server framing, with the actual shared server FLB1 encoder.
Bytes envelope(Json h = header(), Bytes payload = sentinel::flow::encodeFlowBar(sample())) {
    Bytes out{'F','H','E','1'};
    const auto json = h.dump();
    length(out, uint32_t(json.size()));
    out.insert(out.end(), json.begin(), json.end());
    if (!payload.empty()) { length(out, uint32_t(payload.size())); out.insert(out.end(), payload.begin(), payload.end()); }
    return out;
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

TEST(FlowWireClient, HistoryRoundTripsCoreEncoderAndEnvelopeMetadata) {
    const auto out = protocol::clientparse::parseFlowHistory(envelope());
    EXPECT_TRUE(out.history);
    EXPECT_EQ(out.id, 91u);
    EXPECT_EQ(out.oldestMs, 0);
    EXPECT_EQ(out.sealedThroughMs, 120'000);
    ASSERT_EQ(out.bars.size(), 1u);
    EXPECT_EQ(out.bars[0].bar, sample());
    EXPECT_EQ(out.bars[0].revision, 7u);
    EXPECT_TRUE(out.bars[0].complete);
    EXPECT_EQ(out.bars[0].source, Client::FlowSource::Store);
    ASSERT_EQ(out.ranges.size(), 2u);
    EXPECT_EQ(out.ranges[0].status, Client::FlowStatus::Building);
}
TEST(FlowWireClient, LiveRoundTripsCoreEncoder) {
    const auto out = protocol::clientparse::parseFlowLive(live());
    EXPECT_FALSE(out.history);
    EXPECT_EQ(out.id, 17u);
    ASSERT_EQ(out.bars.size(), 1u);
    EXPECT_EQ(out.bars[0].bar, sample());
    EXPECT_EQ(out.bars[0].source, Client::FlowSource::Live);
}
TEST(FlowWireClient, RejectsCorruptionAndMetadataMismatch) {
    auto bytes = envelope();
    bytes.pop_back();
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(bytes), std::exception);
    bytes = envelope(); bytes.push_back(0);
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(bytes), std::exception);
    bytes = envelope(); bytes[4] = bytes[5] = bytes[6] = bytes[7] = 255;
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(bytes), std::exception);
    auto h = header(); h["bars"][0]["bucket_start_ms"] = 0;
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(envelope(h)), std::exception);
    h = header(); h["bars"][0]["revision"] = uint64_t(UINT32_MAX) + 1;
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(envelope(h)), std::exception);
    h = header(); h["sealed_through_ms"] = 60'000;
    EXPECT_THROW(protocol::clientparse::parseFlowHistory(envelope(h)), std::exception);
    auto m = live(); m["bar"] = "@@@";
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
    m = live(); m["timeframe_ms"] = 100;
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
    m = live(); m["source"] = "store";
    EXPECT_THROW(protocol::clientparse::parseFlowLive(m), std::exception);
}
TEST(FlowWireClient, RefusedAndMixedSourcePartialCoarsePage) {
    auto h = header(); h["status"] = "refused"; h["bars"] = Json::array(); h["error"] = "unsupported";
    h["ranges"] = {{{"from_ms", 0}, {"to_ms", 60'000}, {"status", "refused"}}};
    const auto refused = protocol::clientparse::parseFlowHistory(envelope(h, {}));
    EXPECT_EQ(refused.status, Client::FlowStatus::Refused);
    EXPECT_TRUE(refused.bars.empty());
    h = header(); h["sealed_through_ms"] = 90'000;
    h["bars"][0]["source"] = "live"; h["bars"][0]["complete"] = false;
    const auto provisional = protocol::clientparse::parseFlowHistory(envelope(h));
    EXPECT_FALSE(provisional.bars[0].complete);
    EXPECT_EQ(provisional.bars[0].source, Client::FlowSource::Live);
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
    void saturateDecoder(quint64 request) {
        client.m_flowHistoryRequest.store(request);
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
TEST_F(FlowWireClientTest, LiveAndHistoryUseFifoWorkerAndQueuedDelivery) {
    std::vector<Client::FlowBatchPtr> seen;
    QObject::connect(&client, &Client::flowBarsReceived, &client,
                     [&](auto b) { seen.push_back(std::move(b)); }, Qt::QueuedConnection);
    std::promise<void> release;
    holdDecoder(release.get_future().share());
    receive(envelope()); receive(live());
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    EXPECT_TRUE(seen.empty()); // neither decode may run inline on the caller
    release.set_value();
    drain();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen[0]->history);
    EXPECT_FALSE(seen[1]->history);
}
TEST_F(FlowWireClientTest, RequestsAndBusyErrorsAreCorrelated) {
    const auto sub = client.subscribeFlowLive("BTC-USD", 1000);
    client.requestFlowHistory(42, "BTC-USD", 1000, 8000, 9);
    client.unsubscribeFlowLive(sub);
    const auto sent = requests();
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[0]["sub"], sub);
    EXPECT_EQ(sent[1]["req"], 42);
    EXPECT_EQ(sent[1]["end_time_ms"], 8000);
    EXPECT_EQ(sent[2]["type"], "flow_live_unsubscribe");
    bool busy = false;
    QObject::connect(&client, &Client::flowHistoryFailed, &client,
                     [&](quint64 req, const QString&, Client::FlowFailure failure) {
                         busy = req == 42 && failure == Client::FlowFailure::Retryable;
                     });
    receive(Json{{"type", "error"}, {"context", "flow_history_request"}, {"req", 42}, {"message", "1s journal replay busy"}});
    EXPECT_TRUE(busy);
}

TEST_F(FlowWireClientTest, FlowBacklogRefusalReleasesCorrelatedFlightAfterHeatmapRefusal) {
    quint64 failed = 0;
    QObject::connect(&client, &Client::flowHistoryFailed, &client,
                     [&](quint64 req, const QString&, Client::FlowFailure failure) {
                         if (failure == Client::FlowFailure::Retryable) failed = req;
                     }, Qt::QueuedConnection);
    saturateDecoder(123);
    receive(envelope()); drain();
    EXPECT_EQ(failed, 123u);
}

TEST_F(FlowWireClientTest, ClassifiesEveryCurrentServerHistoryError) {
    using Failure = Client::FlowFailure;
    std::vector<Failure> seen;
    QObject::connect(&client, &Client::flowHistoryFailed, &client,
                     [&](quint64 req, const QString&, Failure failure) {
                         if (req == 42) seen.push_back(failure);
                     });
    const std::pair<const char*, Failure> cases[] = {
        {"1s journal replay busy", Failure::Retryable},
        {"history worker queue is full", Failure::Retryable},
        {"one flow history job is already in flight", Failure::Retryable},
        {"flow history frame exceeds 15 MiB cap; reduce limit", Failure::Oversized},
        {"unsupported timeframe", Failure::Fatal},
    };
    for (const auto& [message, expected] : cases) {
        receive(Json{{"type", "error"}, {"context", "flow_history_request"},
                     {"req", 42}, {"message", message}});
        ASSERT_FALSE(seen.empty());
        EXPECT_EQ(seen.back(), expected);
    }
}
TEST_F(FlowWireClientTest, DroppedLiveFrameReportsBucketOnlyOncePerThrottleInterval) {
    std::vector<int64_t> dropped;
    QObject::connect(&client, &Client::flowLiveDropped, &client,
                     [&](quint64 sub, const QString& symbol, int64_t tf, int64_t bucket) {
                         if (sub == 17 && symbol == "BTC-USD" && tf == 60'000)
                             dropped.push_back(bucket);
                     }, Qt::QueuedConnection);
    saturateDecoder(0);
    receive(live()); receive(live());
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped.front(), 60'000);
}
