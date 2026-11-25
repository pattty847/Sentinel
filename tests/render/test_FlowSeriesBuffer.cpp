#include <gtest/gtest.h>
#include "datasources/FlowSeriesBuffer.hpp"
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
Client::FlowRecord bar(uint32_t revision = 1, bool complete = false, Source source = Source::Live,
                       int64_t start = 60'000, int64_t tf = 60'000) {
    Client::FlowRecord out;
    out.bar.startMs = start; out.bar.timeframeMs = tf;
    out.revision = revision; out.complete = complete; out.source = source;
    return out;
}
Client::FlowBatch batch(Client::FlowRecord record = bar()) {
    Client::FlowBatch out;
    out.symbol = "BTC-USD"; out.timeframeMs = record.bar.timeframeMs;
    out.bars.push_back(record);
    return out;
}
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

#include "flow/FlowChunkCodec.hpp"
#include "protocol/FlowChunkWire.hpp"
namespace {
namespace f = sentinel::flow;
namespace w = protocol::chunks;
constexpr int64_t T = 946684800000LL, M = 60000, H = 60*M, D = 24*H;
w::Availability availability(const QString& symbol = "BTC-USD", int hours = 24) {
    recording::FlowChunkStore::Snapshot s; s.epoch = 1; s.liveFromMs = T;
    if (hours) s.levels[0] = {{T, T+hours*H}};
    return w::parseAvailability(protocol::flowchunks::availability("coinbase:" + symbol.toStdString(), s));
}
f::FlowBar candle(int64_t time, int64_t tf = M, int delta = 1) {
    f::FlowBar b; b.startMs = time; b.timeframeMs = tf;
    b.nativeTick = 1; b.priceScale = b.baseScale = b.quoteScale = 1;
    b.open = b.high = b.low = b.close = b.lowRow = 100;
    b.baseVolume = b.buyVol = b.quoteVolume = delta; b.count = 1;
    b.delta = b.maxRowVol = delta; b.rows = {{0, 0, delta, 0, 1}};
    return b;
}
void cacheHour(f::FlowChunkCache& cache, int hour, const QString& symbol = "BTC-USD") {
    const w::Key key{"coinbase:" + symbol.toStdString(), "flow.rows", M, 1, T+hour*H};
    const auto b = candle(key.startMs);
    const auto data = f::chunks::fromBars(std::span(&b, 1), M, key.startMs, 60);
    const auto bytes = f::chunks::encode(data);
    auto decoded = std::make_shared<const f::chunks::FlowChunk>(f::chunks::decode(bytes, key, 60, 1));
    cache.put(key, decoded, 1, uint64_t(hour+1));
}
}
struct FlowDataSourceTest : testing::Test {
    using Json = nlohmann::json;
    int argc = 1; char name[16] = "flow-source"; char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    RemoteGridDataSource source{"127.0.0.1", "1"};
    Client& client() { return *source.streamClient(); }
    void settle() {
        // Cross GUI -> compositor -> transport -> fetcher queued boundaries;
        // no sleeps or sockets, and no worker thread reads GUI members.
        for (int i=0; i<12; ++i) {
            QCoreApplication::processEvents();
            std::promise<void> done;
            source.m_flowChunks->queued([&](auto& fetch) { fetch.pump(); done.set_value(); });
            done.get_future().wait();
        }
        QCoreApplication::processEvents();
    }
    std::vector<Json> requests() {
        settle(); client().m_ioc.restart(); client().m_ioc.poll();
        std::vector<Json> out;
        for (const auto& payload : client().m_writeQueue) out.push_back(Json::parse(payload));
        client().m_writeQueue.clear(); return out;
    }
    void ready() {
        // Model a handshake while keeping the final socket write held.
        client().m_isConnected = true; client().m_writeInFlight = true;
        client().connected(); client().serverConfigReceived(ServerConfig{}); settle(); requests();
    }
    void select(const QString& symbol = "BTC-USD", int64_t tf = M, int64_t from = T, int64_t to = T+H) {
        source.subscribeFlowLive(symbol, tf); source.setFlowViewport(symbol, tf, from, to, -1, true); settle();
    }
    void available(const QString& symbol = "BTC-USD", int hours = 24) {
        client().chunkAvailabilityReceived(availability(symbol, hours)); settle();
    }
    void warm(const QString& symbol = "BTC-USD", int hours = 24) {
        for (int h=0; h<hours; ++h) cacheHour(source.m_flowChunks->cache(), h, symbol);
    }
    void hideFirst() { source.m_flowChunks->cache().hide({"coinbase:BTC-USD", "flow.rows", M, 1, T}); }
    quint64 sub() const { return source.m_flowSubscription; }
    bool replayReady() const { return source.m_flowReplayReady; }
    int retry() const { return source.m_flowRetryMs; }
    int delay() const { return source.m_flowTimer.remainingTime(); }
    void retryNow() { source.m_flowTimer.stop(); source.startFlowLive(); settle(); }
    void live(quint64 id, uint32_t revision, int delta = 1, int64_t tf = M, const QString& symbol = "BTC-USD") {
        auto b = std::make_shared<Client::FlowBatch>(); b->id = id; b->symbol = symbol; b->timeframeMs = tf;
        b->bars.push_back({candle(T, tf, delta), revision, true, Source::Live});
        client().flowBarsReceived(b); settle();
    }
    void barrier() {
        auto b = std::make_shared<Client::FlowBatch>(); b->id = sub(); b->replayReady = true; b->liveFromMs = T;
        client().flowBarsReceived(b); settle();
    }
};
TEST_F(FlowDataSourceTest, AvailabilityPrecedesLiveAndReadyEndsReplay) {
    ready(); select();
    const auto before = requests();
    ASSERT_FALSE(before.empty());
    for (const auto& j : before) EXPECT_EQ(j.at("type"), "chunk_availability_subscribe");
    EXPECT_EQ(sub(), 0u);
    available();
    EXPECT_NE(sub(), 0u); EXPECT_FALSE(replayReady());
    live(sub(), 9); barrier(); EXPECT_TRUE(replayReady());
    ASSERT_NE(source.flowBuffer().series(M), nullptr);
    EXPECT_EQ(source.flowBuffer().series(M)->bars.at(T).revision, 9u);
}
TEST_F(FlowDataSourceTest, RefusalBackoffAndResubscribeResetLiveRevisions) {
    ready(); select(); available("BTC-USD", 0);
    const auto old = sub(); live(old, 99, 7); barrier();
    client().flowLiveFailed(old, "busy"); settle();
    EXPECT_EQ(sub(), 0u); EXPECT_GE(delay(), 0); EXPECT_LE(delay(), 100); EXPECT_EQ(retry(), 200);
    retryNow(); const auto replacement = sub(); ASSERT_NE(old, replacement);
    ASSERT_NE(source.flowBuffer().series(M), nullptr);
    EXPECT_TRUE(source.flowBuffer().series(M)->bars.empty());
    live(old, 100, 8); EXPECT_TRUE(source.flowBuffer().series(M)->bars.empty());
    live(replacement, 1, 2); barrier();
    EXPECT_EQ(source.flowBuffer().series(M)->bars.at(T).bar.delta, 2);
    EXPECT_EQ(retry(), 100);
}
TEST_F(FlowDataSourceTest, WarmPanAndSymbolTimeframeRoundTripNeverRefetch) {
    ready(); warm(); warm("ETH-USD"); select(); available(); requests();
    for (const auto tf : {M, 5*M, H, 4*H, D}) {
        select("BTC-USD", tf, T, T+D);
        ASSERT_NE(source.flowBuffer().series(tf), nullptr);
        EXPECT_EQ(source.flowBuffer().series(tf)->bars.size(), size_t(tf < H ? 24 : D/tf));
        for (const auto& j : requests()) EXPECT_NE(j.at("type"), "chunk_request");
    }
    select("ETH-USD"); available("ETH-USD");
    for (const auto& j : requests()) EXPECT_NE(j.at("type"), "chunk_request");
    select();
    for (const auto& j : requests()) EXPECT_NE(j.at("type"), "chunk_request");
    source.setFlowViewport("BTC-USD", M, T+H, T+2*H, -1, true); settle();
    for (const auto& j : requests()) EXPECT_NE(j.at("type"), "chunk_request");
    EXPECT_EQ(source.flowBuffer().series(M)->bars.begin()->first, T+H);
    EXPECT_EQ(source.flowBuffer().series(M)->coverage.size(), 60u);
}
TEST_F(FlowDataSourceTest, OneSecondIsLiveOnlyAndDisconnectClearsSelection) {
    ready(); select("BTC-USD", 1000, T, T+1000);
    const auto sent = requests();
    ASSERT_EQ(sent.size(), 1u); EXPECT_EQ(sent[0].at("type"), "flow_live_subscribe");
    live(sub(), 1, 3, 1000);
    ASSERT_NE(source.flowBuffer().series(1000), nullptr);
    EXPECT_EQ(source.flowBuffer().series(1000)->bars.at(T).bar.delta, 3);
    client().disconnected(); settle();
    EXPECT_TRUE(source.flowBuffer().symbol().isEmpty());
    select(); EXPECT_EQ(sub(), 0u);
}
TEST(FlowTimingProbe, ReportsCacheFetchAndLiveWithVisibleLatency) {
    FlowTimingProbe probe(true); FlowSeriesBuffer buffer;
    auto b = batch({candle(T), 0, true, Source::Store}); buffer.apply(b, 0);
    probe.viewChanged("BTC-USD", M, T, T+M, 10);
    auto filled = probe.applied(buffer, 10, T+M); ASSERT_TRUE(filled);
    EXPECT_EQ(filled->sinceLastChangeMs, 0); EXPECT_FALSE(filled->fetched); EXPECT_EQ(filled->sealed, 1u);
    probe.viewChanged("BTC-USD", M, T+M, T+2*M, 20);
    EXPECT_FALSE(probe.applied(buffer, 30, T+2*M)); probe.fetched();
    b.bars[0].bar.startMs = T+M; buffer.apply(b, 0);
    filled = probe.applied(buffer, 40, T+2*M); ASSERT_TRUE(filled);
    EXPECT_EQ(filled->sinceLastChangeMs, 20); EXPECT_TRUE(filled->fetched);
    buffer.clear(); b.bars[0].source = Source::Live; buffer.apply(b, 0);
    probe.viewChanged("BTC-USD", M, T+M, T+2*M, 50);
    filled = probe.applied(buffer, 51, T+2*M); ASSERT_TRUE(filled);
    EXPECT_EQ(filled->live, 1u); EXPECT_FALSE(filled->fetched);
}
TEST(FlowSeriesBuffer, SummaryPrefixKeepsCvdAndFooterStableAcrossPans) {
    f::FlowChunkCache cache; f::FlowChunkView state; FlowSeriesBuffer buffer;
    for (int h=0; h<24; ++h) cacheHour(cache, h);
    auto first = std::make_unique<chart::CvdSnapshot>();
    auto pan = std::make_unique<chart::CvdSnapshot>();
    const auto build = [&](int start, chart::CvdSnapshot& out) {
        f::FlowChunkWant want{"coinbase:BTC-USD", "flow.summary", H, T+start*H, T+(start+2)*H, 1};
        buffer.replace("BTC-USD", H, state.compose(cache, want, availability(), T));
        const auto* series = buffer.series(H); ASSERT_NE(series, nullptr);
        EXPECT_EQ(series->bars.size(), 2u);
        for (const auto& [time, record] : series->bars) EXPECT_TRUE(record.bar.rows.empty());
        MappingFrameContext frame; frame.activeTimeframeMs = H; frame.raster.valid = true;
        frame.raster.drawnStartMs = double(want.fromMs); frame.raster.drawnEndMs = double(want.toMs);
        chart::CvdFeed::build(out, *series, frame, chart::CvdReset::UtcDay, 4);
    };
    build(2, *first); build(3, *pan);
    ASSERT_EQ(first->series.count, 2); ASSERT_EQ(pan->series.count, 2);
    EXPECT_EQ(first->series.points[0].value, 3);
    EXPECT_EQ(first->series.points[1].timeMs, pan->series.points[0].timeMs);
    EXPECT_EQ(first->series.points[1].value, pan->series.points[0].value);
    EXPECT_EQ(pan->series.points[0].loadedFromMs, T);
    EXPECT_TRUE(pan->totals[0].valid); EXPECT_EQ(pan->totals[0].delta, 1); EXPECT_EQ(pan->totals[0].volume, 1);
}
TEST_F(FlowDataSourceTest, NoDemandBeforeConfigAndNoChunkAtOrAfterLiveEdge) {
    select(); EXPECT_TRUE(requests().empty());
    client().connected(); settle(); requests();
    select(); EXPECT_TRUE(requests().empty());
    client().serverConfigReceived(ServerConfig{}); settle();
    select("BTC-USD", M, T, T+3*H); requests(); available("BTC-USD", 1);
    const auto sent = requests(); size_t chunks = 0;
    for (const auto& j : sent) if (j.at("type") == "chunk_request") {
        for (const auto& time : j.at("starts")) { ++chunks; EXPECT_EQ(time, T); }
    }
    EXPECT_EQ(chunks, 1u);
}
TEST_F(FlowDataSourceTest, SubscribeBackoffCapsAtFiveSeconds) {
    ready(); select(); available("BTC-USD", 0);
    for (const int expected : {100, 200, 400, 800, 1600, 3200, 5000, 5000}) {
        client().flowLiveFailed(sub(), "busy"); settle();
        EXPECT_GT(delay(), expected-100); EXPECT_LE(delay(), expected);
        retryNow(); ASSERT_NE(sub(), 0u);
    }
}

TEST_F(FlowDataSourceTest, SwitchingAwayAndBackCannotReviveASealedPreview) {
    ready(); warm(); select(); available();
    ASSERT_EQ(source.flowBuffer().series(M)->bars.at(T).source, Source::Store);
    select("ETH-USD"); select(); hideFirst(); available("BTC-USD", 0);
    live(sub(), UINT32_MAX, 99);
    const auto* series = source.flowBuffer().series(M); ASSERT_NE(series, nullptr);
    EXPECT_FALSE(series->bars.contains(T));
    EXPECT_EQ(series->coverage.at(T).status, Status::Building);
}
