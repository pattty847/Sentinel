// Deterministic client/fetch/cache tests. No sockets, recorded roots or GUI.
#include "protocol/SentinelStreamClient.hpp"
#include "protocol/FlowChunkWire.hpp"
#include "flow/FlowChunkClient.hpp"
#include "flow/FlowChunkCodec.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <gtest/gtest.h>
#include <future>
namespace w = protocol::chunks;
namespace f = sentinel::flow;
namespace c = sentinel::flow::chunks;
namespace {
constexpr int64_t T = 946684800000LL, M = 60000, H = 60*M, D = 24*H;
const std::string Id = "coinbase:BTC-USD";
w::Key key(int hour = 0, std::string channel = "flow.rows", int64_t level = M) {
    return {Id, channel, level, channel == "flow.rows" ? 1u : 0u, T+hour*H};
}
w::Availability available(int hours = 1, uint64_t epoch = 1) {
    recording::FlowChunkStore::Snapshot s; s.epoch = epoch; s.liveFromMs = T;
    if (hours) s.levels[0] = {{T, T+hours*H}};
    return w::parseAvailability(protocol::flowchunks::availability(Id, s));
}
f::FlowChunkWant view(int from = 0, int to = 1, int pan = 0, std::string channel = "flow.rows", int64_t tf = M) {
    return {Id, channel, tf, T+from*H, T+to*H, pan};
}
std::shared_ptr<w::Reply> body(w::Key k, uint64_t epoch = 1, int price = 100) {
    auto r = std::make_shared<w::Reply>(); auto& frame = r->frame;
    frame.key = k; frame.epoch = epoch; frame.endMs = frame.committedThroughMs = k.startMs+heatmap::chunkSpanMs(k.levelMs);
    f::FlowBar b; b.startMs=k.startMs; b.timeframeMs=k.levelMs;
    b.nativeTick=1; b.priceScale=b.baseScale=b.quoteScale=1;
    b.open=b.high=b.low=b.close=price; b.lowRow=price;
    b.baseVolume=b.buyVol=b.quoteVolume=b.count=1; b.delta=b.maxRowVol=1; b.rows={{0,0,1,0,1}};
    const auto n = uint16_t(heatmap::chunkSpanMs(k.levelMs)/k.levelMs);
    auto data=c::fromBars(std::span(&b,1),k.levelMs,k.startMs,n);
    if (k.priceStep > 1) data = c::rebin(data, k.priceStep);
    frame.payload=c::encode(data,k.channel=="flow.summary"); frame.contentHash=w::contentHash(frame);
    r->body=std::make_shared<const c::FlowChunk>(c::decode(frame.payload,k,n,epoch)); r->wireBytes=w::encode(frame).size()+14;
    return r;
}
w::ReplyPtr unchanged(const w::ReplyPtr& old, uint64_t epoch) {
    auto r=std::make_shared<w::Reply>(); r->frame=old->frame; r->frame.kind=w::Kind::NotModified;
    r->frame.payload.clear(); r->frame.epoch=epoch; return r;
}
void drain() { for (int i=0;i<24;++i) QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall); }
class Fake final : public w::Transport {
public:
    struct Request { quint64 id; std::vector<w::Key> keys; std::vector<std::optional<uint64_t>> hashes; };
    std::vector<Request> requests;
    std::vector<std::vector<std::string>> subscriptions;
    std::vector<quint64> forgotten;
    quint64 next=0;
    quint64 request(const w::Key& k, std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> hashes) override {
        Request r{++next,{},std::move(hashes)};
        for (auto start:starts) { auto v=k; v.startMs=start; r.keys.push_back(v); }
        requests.push_back(r); return r.id;
    }
    void subscribe(const std::vector<std::string>& ids) override { subscriptions.push_back(ids); }
    void forget(quint64 id) override { forgotten.push_back(id); }
};
class FlowFetch : public testing::Test {
protected:
    int argc=1; char name[16]="flow-fetch"; char* argv[2]={name,nullptr}; QCoreApplication app{argc,argv};
    f::FlowChunkCache cache; Fake transport; int64_t now=1000;
    std::unique_ptr<f::FlowChunkFetcher> fetch;
    void SetUp() override { create(); }
    void create(std::function<size_t(const w::Key&)> estimate={}) {
        f::FlowChunkFetcher::Options o; o.nowMs=[this] {return now;}; o.estimateBytes=std::move(estimate);
        fetch=std::make_unique<f::FlowChunkFetcher>(cache,transport,o);
    }
    void online(w::Availability a=available()) { transport.connected(); transport.availability(std::move(a)); drain(); }
    void answer(Fake::Request r, uint64_t epoch=1) {
        for (const auto& k:r.keys) transport.received(r.id,body(k,epoch)); drain();
    }
};
}
TEST(ChunkClientWire, OutboundRoundTripAndServerAvailability) {
    for (auto channel:{"flow.rows","flow.summary"}) for (auto level:{M,H,D}) {
        auto k=key(0,channel,level);
        std::vector<int64_t> starts;
        for (int i=0;i<64;++i) starts.push_back(T+i*heatmap::chunkSpanMs(level));
        std::vector<std::optional<uint64_t>> hashes(64); hashes[0]=0; hashes[1]=UINT64_MAX;
        const auto j=w::buildRequest(42,k,starts,hashes); protocol::flowchunks::Request parsed;
        ASSERT_FALSE(protocol::flowchunks::parseRequest(j,parsed));
        EXPECT_EQ(parsed.starts,starts); EXPECT_EQ(parsed.have,hashes); EXPECT_EQ(parsed.req,42u);
    }
    for (const auto& ids:std::vector<std::vector<std::string>>{{},{Id},{Id,"kalshi:KXBTC-26"}}) {
        std::vector<std::string> parsed;
        EXPECT_FALSE(protocol::flowchunks::parseSubscribe(w::buildSubscribe(ids),parsed)); EXPECT_EQ(parsed,ids);
    }
    EXPECT_THROW(w::buildSubscribe({Id,Id}),std::exception);
    EXPECT_THROW(w::buildRequest(1,key(),{}),std::exception);
    EXPECT_THROW(w::buildRequest(1,key(),{T},{1,2}),std::exception);
    recording::FlowChunkStore::Snapshot s; s.epoch=UINT64_MAX; s.liveFromMs=T+100;
    s.levels[0]={{T,T+2*H},{T+4*H,T+5*H}}; s.levels[1]={{T,T+D}}; s.levels[2]={{T,T+16*D}};
    const auto a=w::parseAvailability(protocol::flowchunks::availability(Id,s));
    EXPECT_EQ(a.liveFromMs,s.liveFromMs);
    for (auto channel:{"flow.rows","flow.summary"}) {
        const auto& ch=a.channels.at(channel); EXPECT_EQ(ch.epoch,s.epoch);
        for (int n=0;n<3;++n) {
            EXPECT_EQ(ch.levels[n].sealed.size(),s.levels[n].size());
            for (size_t j=0;j<s.levels[n].size();++j) {
                EXPECT_EQ(ch.levels[n].sealed[j].fromMs,s.levels[n][j].fromMs);
                EXPECT_EQ(ch.levels[n].sealed[j].toMs,s.levels[n][j].toMs);
            }
        }
    }
}
TEST(ChunkClientWire, RejectsMalformedSnapshotWithoutPartialPublication) {
    recording::FlowChunkStore::Snapshot s; s.epoch=1; s.levels[0]={{T,T+H}};
    auto good=protocol::flowchunks::availability(Id,s);
    for (int n=0;n<12;++n) {
        auto j=good; auto& l=j["channels"]["flow.rows"]["levels"][0];
        switch(n) {
        case 0:j["wire_version"]=2;break;
        case 1:j["instrument"]="BTC-USD";break;
        case 2:j["live_from_ms"]=UINT64_MAX;break;
        case 3:l["chunk_span_ms"]=1;break;
        case 4:l["sealed"]={{T+1,T+H}};break;
        case 5:l["sealed"]={{T,T+H},{T+H,T+2*H}};break;
        case 6:l["sealed"]={{T,T}};break;
        case 7:l["oldest_ms"]=nullptr;break;
        case 8:l["latest_ms"]=T+H;break;
        case 9:j["channels"]["flow.rows"]["epoch"]="bad";break;
        case 10:j["channels"]["flow.rows"]["levels"].push_back(l);break;
        case 11:j["channels"]=nlohmann::json::array();break;
        }
        EXPECT_THROW(w::parseAvailability(j),std::exception)<<n;
    }
}
TEST(ChunkClientWire, AvailabilityIgnoresUnknownChannelsAndAcceptsKnownSubsets) {
    recording::FlowChunkStore::Snapshot s; s.epoch=1; s.levels[0]={{T,T+H}};
    auto j=protocol::flowchunks::availability(Id,s);
    const auto expected=w::parseAvailability(j);
    j["channels"]["book.near"]={{"epoch","0000000000000002"},{"levels",nlohmann::json::array()}};
    j["channels"]["book.deep"]=nullptr; // Unknown schemas must not affect recognized flow channels.
    EXPECT_EQ(w::parseAvailability(j),expected);
    j["channels"].erase("flow.summary");
    auto& levels=j["channels"]["flow.rows"]["levels"];
    levels.erase(levels.begin()+1,levels.end());
    const auto subset=w::parseAvailability(j);
    ASSERT_EQ(subset.channels.size(),1u);
    ASSERT_EQ(subset.channels.at("flow.rows").levels.size(),1u);
    EXPECT_EQ(subset.channels.at("flow.rows").levels.front(),expected.channels.at("flow.rows").levels.front());
    j["channels"].erase("flow.rows");
    EXPECT_TRUE(w::parseAvailability(j).channels.empty());
}
TEST(FlowChunkPlan, CoarseDivisorWithHoleFallsBackToSealedHoursOnly) {
    auto a=available(48); auto& ch=a.channels.at("flow.rows");
    ch.levels[1].sealed={{T,T+D}}; ch.levels[1].oldestMs=ch.levels[1].latestMs=T;
    ch.levels[0].sealed={{T,T+26*H},{T+27*H,T+48*H}};
    const auto plan=f::planFlowChunks(view(0,72,0,"flow.rows",4*H),a,f::flowChunkEstimate);
    ASSERT_EQ(plan.size(),24u); // 1 day at 1h + 23 sealed hours, one missing hour
    EXPECT_EQ(std::count_if(plan.begin(),plan.end(),[](const auto& p){return p.key.levelMs==H;}),1);
    for(const auto& p:plan) {
        EXPECT_TRUE(a.contains(p.key)); EXPECT_LT(p.key.startMs,T+48*H);
        if(p.key.levelMs==M) EXPECT_GE(p.key.startMs,T+D);
        EXPECT_NE(p.key.startMs,T+26*H);
    }
}
TEST(FlowChunkPlan, PaddingOrderAndCapNeverRemoveVisible) {
    const auto a=available(1000);
    auto plan=f::planFlowChunks(view(10,12,1),a,[](auto&){return 8ull<<20;});
    std::vector<int64_t> starts; for(const auto& p:plan) starts.push_back(p.key.startMs);
    EXPECT_EQ(starts,(std::vector<int64_t>{T+11*H,T+10*H,T+12*H,T+13*H,T+14*H,T+15*H}));
    plan=f::planFlowChunks(view(10,12,-1),a,[](auto&){return 1u;});
    starts.clear(); for(const auto& p:plan) starts.push_back(p.key.startMs);
    EXPECT_EQ(starts.front(),T+11*H); EXPECT_EQ(starts[1],T+10*H); EXPECT_EQ(starts[2],T+9*H);
    EXPECT_EQ(starts[9],T+2*H); EXPECT_EQ(starts[10],T+12*H); EXPECT_EQ(starts.back(),T+15*H);
    plan=f::planFlowChunks(view(0,100),a,[](auto&){return 8ull<<20;});
    EXPECT_EQ(std::count_if(plan.begin(),plan.end(),[](auto& p){return p.tier==0;}),100);
    EXPECT_EQ(plan.size(),104u);
}
TEST(FlowChunkPlan, PaddingStopsBeforeCheaperFartherChunksAtAnotherLevel) {
    auto a=available(96);
    a.channels.at("flow.rows").levels[1].sealed={{T,T+3*D}};
    const auto want=view(0,24,1,"flow.rows",4*H);
    const auto estimate=[](const w::Key& k) -> size_t {
        if (k.levelMs==M) return 64ull << 10; // Hour chunks in the farther, unsealed day.
        return (k.startMs==T+D ? 16ull : 20ull) << 20; // Day chunks at the hour level.
    };
    const auto uncapped=f::planFlowChunks(want,a,estimate,64ull << 20);
    ASSERT_TRUE(std::any_of(uncapped.begin(),uncapped.end(),[](const auto& p) { return p.key.levelMs==M; }));
    const auto capped=f::planFlowChunks(want,a,estimate);
    ASSERT_EQ(capped.size(),2u);
    EXPECT_EQ(capped[0].key,key(0,"flow.rows",H));
    EXPECT_EQ(capped[1].key,key(24,"flow.rows",H));
}
TEST_F(FlowFetch, WaitsForFirstAvailabilityDeduplicatesAndCapsFlight) {
    ASSERT_TRUE(fetch->want(1,view(0,20))); transport.connected(); drain(); EXPECT_TRUE(transport.requests.empty());
    transport.availability(available(20)); drain();
    ASSERT_EQ(transport.requests.size(),1u); EXPECT_EQ(transport.requests[0].keys.size(),8u);
    EXPECT_EQ(transport.requests[0].keys.front(),key(19)); EXPECT_EQ(fetch->stats().inFlightChunks,8u);
    fetch->want(2,view(0,20)); drain(); EXPECT_EQ(transport.requests.size(),1u);
    answer(transport.requests[0]); ASSERT_EQ(transport.requests.size(),2u);
    answer(transport.requests[1]); ASSERT_EQ(transport.requests.size(),3u); answer(transport.requests[2]);
    EXPECT_EQ(fetch->stats().requested,20u); EXPECT_EQ(cache.stats().entries,20u);
    EXPECT_EQ(fetch->stats().inFlightChunks,0u);
    EXPECT_EQ(fetch->stats().cacheHits,0u); // arrival-time rebudgeting is not a cache hit
}
TEST_F(FlowFetch, EstimatedBytesBindBeforeChunkCountAndVisibleSurvivesPaddingCap) {
    fetch.reset(); create([](const auto&){return 3ull<<20;}); online(available(20));
    fetch->want(1,view(0,20)); drain(); EXPECT_EQ(fetch->stats().inFlightChunks,2u);
    EXPECT_EQ(fetch->stats().inFlightBytes,6ull<<20);
    size_t answered=0;
    while(answered<transport.requests.size()) { auto r=transport.requests[answered++]; answer(r); EXPECT_LE(fetch->stats().inFlightBytes,8ull<<20); }
    EXPECT_EQ(cache.stats().entries,20u);
}
TEST_F(FlowFetch, ByteLruPinsWantedAndSymbolSwitchBackUsesCache) {
    online(available(3)); fetch->want(1,view(0,1)); drain(); answer(transport.requests[0]);
    const auto before=transport.requests.size();
    auto eth=view(); eth.instrument="coinbase:ETH-USD"; fetch->want(1,eth); drain();
    fetch->want(1,view(0,1)); drain(); EXPECT_EQ(transport.requests.size(),before);
    cache.setBudget(1); EXPECT_EQ(cache.stats().entries,3u); // padding is wanted too
    fetch->release(1); drain(); EXPECT_EQ(cache.stats().entries,0u);
    const auto a=body(key()), b=body(key(1)), cbody=body(key(2));
    cache.setBudget(2*a->body->storageBytes);
    cache.put(key(),a->body,1,a->frame.contentHash); cache.put(key(1),b->body,1,b->frame.contentHash);
    cache.read(key()); cache.put(key(2),cbody->body,1,cbody->frame.contentHash);
    EXPECT_TRUE(cache.read(key())); EXPECT_FALSE(cache.read(key(1))); EXPECT_TRUE(cache.read(key(2)));
    fetch->hostChanged(); EXPECT_EQ(cache.stats().bytes,0u);
}
TEST_F(FlowFetch, EpochStaleHashConfirmationRevisionAndUnavailableHide) {
    online(); fetch->want(1,view()); drain(); const auto original=body(key());
    transport.received(transport.requests.back().id,original); drain();
    fetch->release(1); transport.availability(available(1,2)); drain();
    EXPECT_TRUE(cache.inspect(key())->stale); EXPECT_EQ(cache.read(key()),original->body);
    EXPECT_EQ(transport.requests.size(),1u);
    fetch->want(1,view()); drain(); ASSERT_EQ(transport.requests.size(),2u);
    EXPECT_EQ(transport.requests.back().hashes[0],original->frame.contentHash);
    transport.received(transport.requests.back().id,unchanged(original,2)); drain();
    EXPECT_EQ(cache.inspect(key())->epoch,2u); EXPECT_FALSE(cache.inspect(key())->stale);
    EXPECT_EQ(cache.read(key()),original->body); // no reallocation on confirmation
    int revised=0,hidden=0;
    QObject::connect(fetch.get(),&f::FlowChunkFetcher::chunkRevised,&app,[&](auto){++revised;},Qt::QueuedConnection);
    QObject::connect(fetch.get(),&f::FlowChunkFetcher::chunkHidden,&app,[&](auto){++hidden;},Qt::QueuedConnection);
    transport.availability(available(1,3)); drain();
    transport.received(transport.requests.back().id,body(key(),3,101)); drain(); EXPECT_EQ(revised,1);
    transport.availability(available(1,4)); drain();
    transport.failed({transport.requests.back().id,key(),"unavailable","fenced"}); drain();
    EXPECT_FALSE(cache.read(key())); EXPECT_EQ(hidden,1);
    const auto count=transport.requests.size(); now+=10000; fetch->pump(); drain(); EXPECT_EQ(transport.requests.size(),count);
    transport.availability(available(0,4)); drain(); transport.availability(available(1,4)); drain();
    ASSERT_EQ(transport.requests.size(),count+1); answer(transport.requests.back(),4); EXPECT_TRUE(cache.read(key()));
    transport.availability(available(0,4)); drain(); EXPECT_FALSE(cache.read(key())); EXPECT_EQ(hidden,2);
}
TEST_F(FlowFetch, BusyDeadlineAndReconnectRequireFreshAvailability) {
    online(); fetch->want(1,view()); drain();
    for(int delay:{100,200,400,800,1600,3200,5000,5000}) {
        const auto count=transport.requests.size(); transport.failed({transport.requests.back().id,key(),"busy","busy"}); drain();
        now+=delay-1; fetch->pump(); drain(); EXPECT_EQ(transport.requests.size(),count);
        ++now; fetch->pump(); drain(); EXPECT_EQ(transport.requests.size(),count+1);
    }
    const auto lost=transport.requests.back(); now+=30000;
    // A reply is late even if the timer event has not been delivered yet.
    transport.received(lost.id,body(key())); drain(); EXPECT_FALSE(cache.read(key()));
    EXPECT_EQ(fetch->stats().inFlightChunks,0u);
    const auto subscriptionsBeforeReconnect=transport.subscriptions.size();
    transport.disconnected(); drain(); transport.connected(); drain();
    ASSERT_EQ(transport.subscriptions.size(),subscriptionsBeforeReconnect+1);
    EXPECT_EQ(transport.subscriptions.back(),std::vector<std::string>{Id});
    EXPECT_EQ(transport.requests.back().id,lost.id); EXPECT_FALSE(fetch->availability(Id));
    transport.availability(available()); drain(); EXPECT_NE(transport.requests.back().id,lost.id);
    transport.received(lost.id,body(key())); drain(); EXPECT_FALSE(cache.read(key()));
    answer(transport.requests.back()); EXPECT_TRUE(cache.read(key()));
}
TEST_F(FlowFetch, RejectsMismatchAndIgnoresPreEpochReply) {
    online(); fetch->want(1,view()); drain();
    transport.received(transport.requests.back().id,body(key(1))); drain();
    EXPECT_EQ(fetch->stats().rejected,1u); EXPECT_FALSE(cache.read(key()));
    fetch->want(1,view()); drain(); const auto old=transport.requests.back();
    transport.availability(available(1,2)); drain(); transport.received(old.id,body(key())); drain();
    EXPECT_FALSE(cache.read(key())); now+=100; fetch->pump(); drain(); answer(transport.requests.back(),2);
    EXPECT_EQ(cache.inspect(key())->epoch,2u);
}
TEST_F(FlowFetch, RowsAtStoredStepSatisfySummaryWithoutAnotherRequest) {
    online(); fetch->want(1,view()); drain(); auto stored=key(); stored.priceStep=8;
    transport.received(transport.requests.back().id,body(stored)); drain();
    ASSERT_TRUE(cache.read(key())); EXPECT_EQ(cache.read(key())->key.priceStep,8u);
    fetch->want(1,view(0,1,0,"flow.summary")); drain(); EXPECT_EQ(transport.requests.size(),1u);
    EXPECT_EQ(cache.read(key(0,"flow.summary")),cache.read(key()));
}
TEST_F(FlowFetch, SummaryPaddingBudgetsTheRowsBodiesItActuallyRetains) {
    fetch.reset(); create([](const auto& k) { return k.channel == "flow.rows" ? 8ull << 20 : 1ull; });
    for (int hour = 0; hour < 20; ++hour) {
        const auto r = body(key(hour));
        cache.put(key(hour), r->body, 1, r->frame.contentHash);
    }
    online(available(20)); fetch->want(1,view(10,12,1,"flow.summary")); drain();
    EXPECT_EQ(fetch->stats().wanted,6u); // two visible + four 8 MiB padding bodies
    EXPECT_TRUE(transport.requests.empty());
}
TEST_F(FlowFetch, NewDecodedSizeRebudgetsPaddingWithoutAnotherWant) {
    bool decoded = false;
    fetch.reset(); create([&](const auto& k) { return decoded && k == key(11) ? 32ull << 20 : 1ull; });
    online(available(20)); fetch->want(1,view(10,11,1)); drain();
    ASSERT_EQ(fetch->stats().wanted,7u);
    const auto request = transport.requests.front();
    ASSERT_NE(std::find(request.keys.begin(),request.keys.end(),key(11)),request.keys.end());
    decoded = true; // transport decoding replaces this padding key's cold estimate
    transport.received(request.id,body(key(11))); drain();
    EXPECT_EQ(fetch->stats().wanted,2u); // visible + the now-known 32 MiB padding key
}

struct FlowChunkClientTest : testing::Test {
    int argc=1; char name[16]="flow-client"; char* argv[2]={name,nullptr}; QCoreApplication app{argc,argv};
    SentinelStreamClient client{"127.0.0.1","1"};
    std::vector<w::ReplyPtr> seen; std::vector<w::Failure> errors;
    void SetUp() override {
        QObject::connect(&client,&SentinelStreamClient::chunkReceived,&app,[&](quint64,w::ReplyPtr r){seen.push_back(r);},Qt::QueuedConnection);
        QObject::connect(&client,&SentinelStreamClient::chunkFailed,&app,[&](w::Failure f){errors.push_back(f);},Qt::QueuedConnection);
    }
    void receive(std::vector<uint8_t> bytes) { client.handleBinaryMessage(std::make_shared<std::vector<uint8_t>>(std::move(bytes))); }
    void flush() {
        std::promise<void> done; net::post(*client.m_decodePool,[&]{done.set_value();}); done.get_future().wait(); drain();
    }
    std::vector<nlohmann::json> writes() {
        client.m_ioc.restart(); client.m_ioc.poll(); std::vector<nlohmann::json> out;
        for(const auto& text:client.m_writeQueue) out.push_back(nlohmann::json::parse(text)); client.m_writeQueue.clear(); return out;
    }
    void handshake() {
        // Exercise the real handshake completion on its strand. Hold writes and
        // stop at connected() so this fixture never starts socket I/O.
        const auto stop=QObject::connect(&client,&SentinelStreamClient::connected,&app,[&] {
            client.m_running=false;
        },Qt::DirectConnection);
        client.m_ioc.restart();
        net::post(client.m_strand,[&] {
            client.m_ws=std::make_unique<SentinelStreamClient::WebSocket>(client.m_strand,client.m_sslCtx);
            client.m_running=true; client.m_writeInFlight=true;
            client.onHandshake({});
            client.m_isConnected=false; client.m_writeInFlight=false;
        });
        client.m_ioc.poll(); QObject::disconnect(stop);
    }
    void connectedState(bool online) { client.m_isConnected = online; }
    void saturate(unsigned refusals=2) { client.m_decodeBacklogFrames=client.kMaxDecodeBacklogFrames; client.m_decodeRefusals=refusals; }
    void unsaturate() { client.m_decodeBacklogFrames=0; }
    void postDecode(std::function<void()> fn) { net::post(*client.m_decodePool,std::move(fn)); }
    void receiveJson(const nlohmann::json& j) { client.handleMessage(j.dump()); }
};
TEST_F(FlowChunkClientTest, RealClientWritesAcceptedRequestsAndResendsReplacementSubscription) {
    client.requestChunks(key(),{T},{uint64_t(0)}); auto sent=writes(); ASSERT_EQ(sent.size(),1u);
    protocol::flowchunks::Request parsed; EXPECT_FALSE(protocol::flowchunks::parseRequest(sent[0],parsed)); EXPECT_EQ(parsed.have[0],0u);
    client.subscribeChunkAvailability({Id}); writes(); handshake(); sent=writes();
    ASSERT_EQ(sent.size(),1u); std::vector<std::string> ids;
    EXPECT_FALSE(protocol::flowchunks::parseSubscribe(sent[0],ids)); EXPECT_EQ(ids,std::vector<std::string>{Id});
    client.subscribeChunkAvailability({}); writes(); handshake(); sent=writes();
    ASSERT_EQ(sent.size(),1u); EXPECT_TRUE(sent[0]["instruments"].empty());
}
TEST_F(FlowChunkClientTest, DecodeIsOffCallerThreadAndMalformedOrMismatchedFramesAreCounted) {
    std::promise<std::thread::id> decodedOn;
    QObject::connect(&client,&SentinelStreamClient::chunkReceived,&app,
        [&](quint64,w::ReplyPtr){decodedOn.set_value(std::this_thread::get_id());},Qt::DirectConnection);
    const auto id=client.requestChunks(key(),{T});
    std::promise<void> release,entered; auto gate=release.get_future().share();
    postDecode([&]{entered.set_value();gate.wait();}); entered.get_future().wait();
    const auto good=body(key()); receive(heatmap::encodeChunkEnvelope(id,w::encode(good->frame)));
    drain(); EXPECT_TRUE(seen.empty()); release.set_value(); flush();
    EXPECT_NE(decodedOn.get_future().get(),std::this_thread::get_id());
    ASSERT_EQ(seen.size(),1u); EXPECT_EQ(c::toData(*seen[0]->body),c::toData(*good->body));
    for(bool mismatch:{false,true}) {
        const auto req=client.requestChunks(key(),{T});
        auto bytes=heatmap::encodeChunkEnvelope(req,w::encode(body(mismatch?key(1):key())->frame));
        if(!mismatch) bytes.pop_back(); receive(std::move(bytes)); flush();
    }
    EXPECT_EQ(client.chunkRejectedCount(),2u); EXPECT_EQ(errors.size(),2u); EXPECT_EQ(seen.size(),1u);
}
TEST_F(FlowChunkClientTest, RejectsLateDecodeAcrossDisconnectAndRetiresForgottenIds) {
    const auto id=client.requestChunks(key(),{T}); client.forgetChunks(id);
    receive(heatmap::encodeChunkEnvelope(id,w::encode(body(key())->frame))); flush(); EXPECT_TRUE(seen.empty());
    const auto next=client.requestChunks(key(),{T});
    std::promise<void> release; auto gate=release.get_future().share(); postDecode([gate]{gate.wait();});
    receive(heatmap::encodeChunkEnvelope(next,w::encode(body(key())->frame)));
    client.disconnectFromServer(); release.set_value(); flush(); EXPECT_TRUE(seen.empty());
}
TEST_F(FlowChunkClientTest, ServiceRunsQueuedWantsOnDedicatedThread) {
    f::FlowChunkClient service([&](QObject* parent){auto* t=new Fake;t->setParent(parent);return t;});
    std::promise<QThread*> done;
    service.queued([&](auto& fetch){EXPECT_TRUE(fetch.want(1,view()));done.set_value(QThread::currentThread());});
    EXPECT_NE(done.get_future().get(),app.thread());
}

TEST_F(FlowChunkClientTest, AvailabilityPublishesTypedSnapshotAndCountsMalformedInput) {
    int count=0, notices=0;
    QObject::connect(&client,&SentinelStreamClient::errorOccurred,&app,[&](const auto&) { ++notices; },Qt::QueuedConnection);
    QObject::connect(&client,&SentinelStreamClient::chunkAvailabilityReceived,&app,[&](const auto& a){
        ++count; EXPECT_EQ(a,available());
    },Qt::QueuedConnection);
    recording::FlowChunkStore::Snapshot snapshot; snapshot.epoch=1; snapshot.liveFromMs=T; snapshot.levels[0]={{T,T+H}};
    auto j=protocol::flowchunks::availability(Id,snapshot);
    j["channels"]["book.near"]={{"epoch","0000000000000002"}};
    receiveJson(j); drain(); EXPECT_EQ(count,1);
    j["channels"]["flow.rows"]["epoch"]="invalid";
    for (int n=0;n<8;++n) receiveJson(j);
    drain();
    EXPECT_EQ(count,1); EXPECT_EQ(client.chunkRejectedCount(),8u); EXPECT_EQ(notices,0);
}
TEST_F(FlowChunkClientTest, AdapterSecondConnectionAndHostReplacementIgnoreOldReplies) {
    SentinelStreamClient replacement("127.0.0.1","2");
    w::ClientTransport transport(client); f::FlowChunkCache cache; f::FlowChunkFetcher fetch(cache,transport);
    client.connected(); client.chunkAvailabilityReceived(available()); drain();
    fetch.want(1,view()); drain(); EXPECT_EQ(fetch.stats().inFlightChunks,1u);
    client.connected(); drain(); EXPECT_EQ(fetch.stats().inFlightChunks,0u);
    client.chunkReceived(1,body(key())); drain(); EXPECT_FALSE(cache.read(key()));
    client.chunkAvailabilityReceived(available()); drain(); EXPECT_EQ(fetch.stats().inFlightChunks,1u);
    client.chunkReceived(2,body(key())); transport.setClient(replacement); drain(); EXPECT_FALSE(cache.read(key()));
    replacement.connected(); replacement.chunkAvailabilityReceived(available()); drain();
    replacement.chunkReceived(1,body(key())); drain(); EXPECT_TRUE(cache.read(key()));
}
TEST_F(FlowFetch, AdmissionOrderIsGlobalAcrossChartsAndPaddingNeverDisplacesVisible) {
    online(available(40));
    fetch->want(1,view(10,11,1)); fetch->want(2,view(20,24,-1)); drain();
    std::vector<w::Key> sent;
    for(const auto& r:transport.requests) sent.insert(sent.end(),r.keys.begin(),r.keys.end());
    ASSERT_EQ(sent.size(),8u);
    EXPECT_EQ(sent[0],key(23)); EXPECT_EQ(sent[1],key(22)); EXPECT_EQ(sent[2],key(21));
    EXPECT_EQ(sent[3],key(20)); EXPECT_EQ(sent[4],key(10));
}
TEST_F(FlowFetch, NinthInstrumentIsRejectedWithoutReplacingExistingWant) {
    for(int n=0;n<8;++n) { auto v=view();v.instrument="coinbase:S"+std::to_string(n);EXPECT_TRUE(fetch->want(n,v)); }
    auto ninth=view();ninth.instrument="coinbase:NINTH";EXPECT_FALSE(fetch->want(9,ninth));
    EXPECT_EQ(transport.subscriptions.back().size(),8u);
    EXPECT_TRUE(fetch->want(0,ninth)); EXPECT_EQ(transport.subscriptions.back().size(),8u);
}
TEST_F(FlowChunkClientTest, HeatmapRepliesDoNotRetirePendingFlowRequests) {
    const auto id=client.requestChunks(key(),{T});
    heatmap::ChunkFrame heat; heat.kind=heatmap::ChunkKind::NotModified;
    heat.key={"BTC-USD","hmc2.deep",M,T}; heat.state={true,T+H,0}; heat.contentHash=1;
    receive(heatmap::encodeChunkEnvelope(100,heatmap::encodeChunk(heat))); flush();
    receive(heatmap::encodeChunkEnvelope(id,w::encode(body(key())->frame))); flush();
    EXPECT_EQ(seen.size(),1u); EXPECT_TRUE(errors.empty());
}

TEST_F(FlowFetch, LateUnavailableCannotHideStaleBodyBeforeDeadlineTimerRuns) {
    online(); fetch->want(1,view()); drain(); answer(transport.requests.back());
    transport.availability(available(1,2)); drain(); const auto request=transport.requests.back();
    now+=30000; transport.failed({request.id,key(),"unavailable","late fence"}); drain();
    EXPECT_TRUE(cache.read(key())); EXPECT_TRUE(cache.inspect(key())->stale);
    EXPECT_EQ(fetch->stats().inFlightChunks,0u); now+=100;fetch->pump();drain();
    EXPECT_NE(transport.requests.back().id,request.id);
}

TEST_F(FlowChunkClientTest, DuplicateStartsEachKeepTheirCorrelatedReply) {
    const auto id=client.requestChunks(key(),{T,T});
    const auto bytes=heatmap::encodeChunkEnvelope(id,w::encode(body(key())->frame));
    receive(bytes); receive(bytes); flush();
    EXPECT_EQ(seen.size(),2u); EXPECT_TRUE(errors.empty());
}
TEST_F(FlowChunkClientTest, UncorrelatedRefusalsDoNotCancelPendingFlowChunks) {
    w::ClientTransport transport(client);
    std::vector<quint64> delivered;
    int heatmapErrors=0;
    QObject::connect(&transport,&w::Transport::received,&app,[&](quint64 id,w::ReplyPtr) { delivered.push_back(id); },Qt::QueuedConnection);
    QObject::connect(&client,&SentinelStreamClient::heatmapChunkFailed,&app,[&](const auto&) { ++heatmapErrors; },Qt::QueuedConnection);
    client.connected(); drain();
    const auto first=transport.request(key(),{T},{});
    const auto second=transport.request(key(1),{T+H},{});
    const auto sent=writes(); ASSERT_EQ(sent.size(),2u);
    receive({0}); // Too short to identify a channel or request.
    saturate(0); receive(std::vector<uint8_t>(18,0)); unsaturate(); flush();
    EXPECT_EQ(heatmapErrors,2); EXPECT_TRUE(errors.empty()); EXPECT_EQ(client.chunkRejectedCount(),0u);
    receive(heatmap::encodeChunkEnvelope(sent[0].at("req").get<uint64_t>(),w::encode(body(key())->frame)));
    receive(heatmap::encodeChunkEnvelope(sent[1].at("req").get<uint64_t>(),w::encode(body(key(1))->frame)));
    flush(); EXPECT_EQ(delivered,(std::vector<quint64>{first,second}));
}
TEST_F(FlowChunkClientTest, FlowChunkBacklogRefusalIsIndependentOfHeatmapRefusalsAndCountsEachDrop) {
    const auto id=client.requestChunks(key(),{T});
    const auto bytes=heatmap::encodeChunkEnvelope(id,w::encode(body(key())->frame));
    saturate(); receive(bytes); receive(bytes); unsaturate(); flush();
    ASSERT_EQ(errors.size(),1u); EXPECT_EQ(errors[0].requestId,id); EXPECT_EQ(errors[0].code,"client_overloaded");
    EXPECT_EQ(client.chunkRejectedCount(),2u);
}

TEST_F(FlowFetch, StaleOrHiddenRowsDoNotCaptureSummaryDemand) {
    for (const bool hidden : {false, true}) {
        fetch->release(1); cache.clear(); transport.requests.clear();
        const auto rows = body(key());
        cache.put(key(), rows->body, 1, rows->frame.contentHash);
        online(available(1, 2));
        if (hidden) cache.hide(key());
        EXPECT_FALSE(cache.read(key(0, "flow.summary")));
        fetch->want(1, view(0, 1, 0, "flow.summary")); drain();
        ASSERT_EQ(transport.requests.size(), 1u);
        EXPECT_EQ(transport.requests.back().keys.front(), key(0, "flow.summary"));
        EXPECT_FALSE(transport.requests.back().hashes.front());
        answer(transport.requests.back(), 2);
        EXPECT_TRUE(cache.read(key(0, "flow.summary")));
    }
}
TEST_F(FlowFetch, UnavailableRowsDoNotBlockSummaryAndSummaryWantRetriesItsOwnFailure) {
    online(); fetch->want(1, view()); drain();
    transport.failed({transport.requests.back().id, key(), "unavailable", "rows unavailable"}); drain();
    fetch->want(1, view(0, 1, 0, "flow.summary")); drain();
    ASSERT_EQ(transport.requests.size(), 2u);
    EXPECT_EQ(transport.requests.back().keys.front(), key(0, "flow.summary"));
    transport.failed({transport.requests.back().id, key(0, "flow.summary"), "build_failed", "retry on want"}); drain();
    fetch->want(1, view(0, 1, 0, "flow.summary")); drain();
    ASSERT_EQ(transport.requests.size(), 3u);
    answer(transport.requests.back());
    EXPECT_TRUE(cache.read(key(0, "flow.summary")));
}
TEST_F(FlowChunkClientTest, ZeroIdSckBacklogRefusalKeepsOtherRequestsPending) {
    w::ClientTransport transport(client);
    std::vector<quint64> delivered;
    QObject::connect(&transport, &w::Transport::received, &app,
        [&](quint64 id, w::ReplyPtr) { delivered.push_back(id); }, Qt::QueuedConnection);
    client.connected(); drain();
    const auto local = transport.request(key(), {T}, {});
    const auto sent = writes(); ASSERT_EQ(sent.size(), 1u);
    auto uncorrelated = heatmap::encodeChunkEnvelope(1, w::encode(body(key())->frame));
    std::fill(uncorrelated.begin() + 6, uncorrelated.begin() + 14, 0);
    saturate(0); receive(std::move(uncorrelated)); unsaturate(); flush();
    EXPECT_TRUE(errors.empty());
    receive(heatmap::encodeChunkEnvelope(sent[0].at("req").get<uint64_t>(), w::encode(body(key())->frame)));
    flush(); EXPECT_EQ(delivered, (std::vector<quint64>{local}));
}

TEST_F(FlowChunkClientTest, AdapterFetchesWhetherAttachedBeforeOrAfterConnection) {
    for (const bool alreadyConnected : {false, true}) {
        connectedState(alreadyConnected);
        w::ClientTransport transport(client);
        f::FlowChunkCache cache;
        f::FlowChunkFetcher fetch(cache, transport);
        fetch.want(1, view());
        if (!alreadyConnected) { connectedState(true); client.connected(); }
        drain(); client.chunkAvailabilityReceived(available()); drain();
        EXPECT_EQ(fetch.stats().inFlightChunks, 1u);
        connectedState(false);
    }
}
TEST_F(FlowChunkClientTest, ServiceDestructionRetiresRequestsWhileClientStillLives) {
    connectedState(true);
    {
        f::FlowChunkClient service([&](QObject* parent) { return new w::ClientTransport(client, parent); });
        service.queued([](auto& fetch) { fetch.want(1, view()); });
        std::promise<void> seeded;
        service.queued([&](auto&) { seeded.set_value(); });
        seeded.get_future().wait();
        client.chunkAvailabilityReceived(available());
        // Adapter and fetcher each receive a queued hop; drain through both.
        for (int turn = 0; turn < 3; ++turn) {
            std::promise<void> pumped;
            service.queued([&](auto& fetch) { fetch.pump(); pumped.set_value(); });
            pumped.get_future().wait();
        }
    }
    // Teardown joins the flow thread before the stream client's members can die.
    connectedState(false);
    auto sent = writes();
    std::erase_if(sent, [](const auto& j) { return j.at("type") != "chunk_request"; });
    ASSERT_EQ(sent.size(), 1u);
    receive(heatmap::encodeChunkEnvelope(sent[0].at("req").get<uint64_t>(), w::encode(body(key())->frame)));
    flush(); EXPECT_TRUE(seen.empty());
    connectedState(false);
}

#include "flow/FlowChunkView.hpp"
#include "heatmap/ChartSurface.hpp"
#include "flow/FlowCompose.hpp"
TEST(FlowChunkView, ComposesRowsAndSummaryAtEveryChartTimeframe) {
    for (const auto tf : {M, 5*M, H, 4*H, D}) for (const auto channel : {"flow.rows", "flow.summary"}) {
        f::FlowChunkCache cache;
        auto a = available(24);
        std::vector<f::FlowBar> input;
        for (int hour = 0; hour < 24; ++hour) {
            const auto r = body(key(hour, channel));
            cache.put(key(hour, channel), r->body, 1, r->frame.contentHash);
            auto rows = c::toBars(*body(key(hour))->body);
            if (std::string_view(channel) == "flow.summary") {
                rows[0].rows.clear(); rows[0].maxRowVol = 0;
            }
            input.push_back(rows[0]);
        }
        f::FlowChunkView viewState;
        const auto result = viewState.compose(cache, view(0, 24, 0, channel, tf), a);
        EXPECT_EQ(result.buckets.size(), size_t(D/tf));
        for (const auto& [time, bucket] : result.buckets) {
            EXPECT_EQ(bucket.status, c::Status::Complete);
            std::vector<f::FlowBar> bars;
            for (const auto& bar : input) if (bar.startMs >= time && bar.startMs < time+tf) bars.push_back(bar);
            if (bars.empty()) EXPECT_FALSE(bucket.value);
            else {
                ASSERT_TRUE(bucket.value);
                EXPECT_EQ(bucket.value->bar, f::composeFlowBars(bars, tf));
                EXPECT_TRUE(bucket.value->sealed);
            }
        }
    }
}
TEST(FlowChunkView, SealedReplacesAnyLiveRevisionAndNeverReturnsToLive) {
    f::FlowChunkCache cache; f::FlowChunkView state;
    auto live = c::toBars(*body(key(), 1, 200)->body).front();
    state.live({live, UINT32_MAX, true, false}, c::Status::Complete);
    auto want = view(); want.toMs = T+M;
    auto a = available();
    EXPECT_EQ(state.compose(cache, want, a).buckets.at(T).value->bar.close, 200);
    const auto sealed = body(key()); cache.put(key(), sealed->body, 1, sealed->frame.contentHash);
    EXPECT_EQ(state.compose(cache, want, a).buckets.at(T).value->bar.close, 100);
    cache.hide(key());
    state.live({live, UINT32_MAX, true, false}, c::Status::Complete);
    const auto fenced = state.compose(cache, want, a);
    EXPECT_FALSE(fenced.buckets.at(T).value);
    EXPECT_EQ(fenced.buckets.at(T).status, c::Status::Building);
}
TEST(FlowChunkView, ReplayResetAndRetentionWithdrawPreviewWithoutInventingHistory) {
    f::FlowChunkCache cache; f::FlowChunkView state;
    auto live = c::toBars(*body(key())->body).front();
    auto want = view(); want.toMs = T+M;
    auto a = available(0); a.liveFromMs = T;
    state.live({live, 99, true, false}, c::Status::Complete);
    state.resetLive();
    EXPECT_FALSE(state.compose(cache, want, a).buckets.at(T).value);
    live.close = 101;
    state.live({live, 1, true, false}, c::Status::Complete);
    EXPECT_EQ(state.compose(cache, want, a).buckets.at(T).value->bar.close, 101);
    state.retention(T+H);
    a.liveFromMs = T+H;
    // The recorded range precedes live retention, but its hour has not sealed.
    a.channels.at("flow.rows").levels[0].oldestMs = T-H;
    const auto expired = state.compose(cache, want, a);
    EXPECT_FALSE(expired.buckets.at(T).value);
    EXPECT_EQ(expired.buckets.at(T).status, c::Status::Building);
}
TEST(FlowChunkView, SummaryAnchorSurvivesPanWithoutRetainingPrefixBars) {
    f::FlowChunkCache cache; f::FlowChunkView state;
    auto a = available(6);
    for (int h = 0; h < 6; ++h) {
        const auto r = body(key(h, "flow.summary"));
        cache.put(key(h, "flow.summary"), r->body, 1, r->frame.contentHash);
    }
    const auto first = state.compose(cache, view(2, 4, 0, "flow.summary", H), a, T);
    const auto pan = state.compose(cache, view(3, 5, 1, "flow.summary", H), a, T);
    ASSERT_EQ(first.buckets.size(), 2u); ASSERT_EQ(pan.buckets.size(), 2u);
    EXPECT_EQ(first.prefixDelta, 2); EXPECT_EQ(pan.prefixDelta, 3);
    EXPECT_TRUE(first.prefixComplete); EXPECT_TRUE(pan.prefixComplete);
    EXPECT_EQ(first.prefixDelta + first.buckets.at(T+2*H).value->bar.delta, pan.prefixDelta);
}
TEST(FlowChunkView, MissingOlderHourIsBuildingDuringNewestFirstRecovery) {
    f::FlowChunkCache cache; f::FlowChunkView state;
    auto a = available(0); a.liveFromMs = T+6*H;
    for (auto& [channel, values] : a.channels) {
        values.levels[0].sealed = {{T+H, T+2*H}};
        values.levels[0].oldestMs = values.levels[0].latestMs = T+H;
    }
    auto want = view(); want.toMs = T+M;
    const auto snapshot = state.compose(cache, want, a);
    EXPECT_EQ(snapshot.buckets.at(T).status, c::Status::Building);
    EXPECT_FALSE(snapshot.buckets.at(T).value);
}
TEST(FlowChunkView, SealedEmptySlotWithdrawsLiveAndViewportIsBounded) {
    f::FlowChunkCache cache; f::FlowChunkView state;
    auto preview = c::toBars(*body(key())->body).front(); preview.startMs += M;
    state.live({preview, UINT32_MAX, true, false}, c::Status::Complete);
    const auto chunk = body(key()); cache.put(key(), chunk->body, 1, chunk->frame.contentHash);
    auto want = view(); want.fromMs += M; want.toMs = want.fromMs+M;
    const auto empty = state.compose(cache, want, available());
    ASSERT_EQ(empty.buckets.size(), 1u);
    EXPECT_FALSE(empty.buckets.begin()->second.value);
    EXPECT_EQ(empty.buckets.begin()->second.status, c::Status::Complete);
    want = view(0, 200);
    const auto bounded = state.compose(cache, want, available(200));
    EXPECT_EQ(bounded.buckets.size(), size_t(heatmap::kMaxChartSurfaceBars));
    EXPECT_EQ(bounded.buckets.begin()->first, want.toMs-int64_t(heatmap::kMaxChartSurfaceBars)*M);
}
