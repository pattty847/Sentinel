// Real request handler, history pool, admission and executor/write queue; only
// the final socket write is replaced. Synthetic roots, no listener or services.
#define SENTINEL_FLOW_CHUNK_TEST
#include "protocol/SentinelStreamServer.cpp"
#include "roller/fixtures/FlowJournal.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "protocol/ChunkClientTransport.hpp"
#include "flow/FlowChunkFetcher.hpp"
#include "flow/FlowChunkCodec.hpp"
#include <QEvent>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <gtest/gtest.h>
#include <future>
using namespace std::chrono_literals;
using Json = nlohmann::json;
struct FlowChunkWireTest {
    static std::shared_ptr<Session> start(SentinelStreamServer& server, ServerDataModel& model,
                                         std::function<void(std::string,bool)> sink) {
        server.m_historyWorkers = std::make_unique<net::thread_pool>(2); server.m_running = true;
        auto session = std::make_shared<Session>(tcp::socket(net::make_strand(server.m_ioc)), server.m_sslCtx, model, &server);
        session->chunkWriteForTest_ = std::move(sink);
        server.m_thread = std::thread([&server, guard = net::make_work_guard(server.m_ioc)] { server.m_ioc.run(); });
        return session;
    }
    template<class F> static void on(Session& s, F f) {
        std::promise<void> done;
        net::post(s.ws_.get_executor(), [&] { f(s); done.set_value(); });
        ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
    }
    static void sink(Session& s, std::function<void(std::string,bool)> fn) {
        on(s,[&](auto& v) { v.chunkWriteForTest_=std::move(fn); });
    }
    static void request(Session& s, const Json& j) { on(s, [&](auto& v) { v.handleFlowChunkRequest(j); }); }
    static void subscribe(Session& s, const Json& j) { on(s, [&](auto& v) { v.handleFlowAvailabilitySubscribe(j); }); }
    static void poll(Session& s, bool advance = false) {
        on(s, [&](auto& v) {
            for (auto& [id, state] : v.flowAvailability_) {
                if (advance) state->lastSent -= 1s;
                v.pushFlowAvailability(id);
            }
        });
    }
    static void budget(Session& s, size_t jobs, size_t bytes) { on(s, [&](auto& v) { v.chunkJobs_ = jobs; v.chunkBytes_ = bytes; }); }
    static void after(SentinelStreamServer& s, std::function<void(const protocol::chunks::Key&)> fn) { s.m_afterFlowChunkServeForTest = std::move(fn); }
    static bool submit(SentinelStreamServer& s, std::function<void()> fn) { return s.submitHistoryTask(std::move(fn)); }
    static void detach(SentinelStreamServer& s) { s.m_flowChunks.reset(); }
    static void drain(SentinelStreamServer& s, Session& session) {
        const auto until = std::chrono::steady_clock::now()+5s;
        while (s.m_pendingHistoryTasks.load() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        EXPECT_EQ(s.m_pendingHistoryTasks.load(), 0u); on(session, [](auto&) {});
    }
};
struct FlowChunkClientTest {
    static void receive(SentinelStreamClient& c, const std::string& bytes, bool binary) {
        if (binary) c.handleBinaryMessage(std::make_shared<std::vector<uint8_t>>(bytes.begin(),bytes.end()));
        else c.handleMessage(bytes);
    }
    static std::vector<Json> writes(SentinelStreamClient& c, bool subscription = false) {
        c.m_ioc.restart(); c.m_ioc.poll();
        if (subscription) c.resendChunkSubscription();
        std::vector<Json> out;
        for (const auto& s : c.m_writeQueue) out.push_back(Json::parse(s));
        c.m_writeQueue.clear(); return out;
    }
};
namespace {
using namespace flow_fixture;
namespace fs = std::filesystem;
namespace wire = protocol::chunks;
constexpr int64_t Day = 24 * Hour;
const std::string Id = "coinbase:BTC-USD";
class FlowChunkWire : public testing::Test {
protected:
    int argc = 1; char name[20] = "flow-chunk-wire"; char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv}; QTemporaryDir dir;
    fs::path base = dir.path().toStdString(), raw = base/"raw", hmc = base/"hmc";
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<recording::FlowStore> source;
    std::unique_ptr<recording::FlowChunkStore> store;
    std::unique_ptr<SentinelStreamServer> server;
    std::shared_ptr<Session> session;
    std::mutex mutex; std::condition_variable cv;
    std::vector<wire::Envelope> binary;
    std::vector<Json> text;
    std::thread::id readThread;
    std::function<void()> beforeEpochWrite;
    virtual int64_t fixtureThrough() const { return Epoch+Day; }
    virtual bool fillDays() const { return false; }
    void SetUp() override {
        fs::create_directories(raw); fs::create_directories(hmc);
        Journal j(raw); j.snapshot(-Minute); j.trades(1000, {trade(100,"1",100000)}); j.heartbeat(Hour+15000);
        if (fillDays()) for (int64_t h=1; Epoch+h*Hour<fixtureThrough(); ++h) {
            if (h % 24 == 0) j.snapshot(h*Hour+16000);
            j.trades(h*Hour+20000, {trade(h*Hour+100, std::to_string(h+1), 100000+h)});
            j.heartbeat((h+1)*Hour+15000);
        }
        j.close();
        recording::FlowStore::Options o; o.root=base/"fld"; o.hmc2Root=hmc; o.journalRoot=raw;
        o.hourThrough=[this](const std::string&) { return fixtureThrough(); };
        o.symbols={"BTC-USD"}; o.lowerPriority=false; o.nowMs=[this] { return fixtureThrough()+(fillDays()?Hour:0); };
        source=std::make_unique<recording::FlowStore>(o);
        recording::FlowChunkStore::Options c; c.root=base/"flc"; c.lowerPriority=false; c.rules.sealWindowMs=15000;
        c.afterSectionReadForTest=[&] { readThread=std::this_thread::get_id(); };
        c.beforeEpochWriteForTest=[&] { if (beforeEpochWrite) beforeEpochWrite(); };
        store=std::make_unique<recording::FlowChunkStore>(*source,c); ASSERT_EQ(store->runPass().failed,0u);
        config.recording.enabled=false; config.rollerShadow.enabled=false;
        model=std::make_unique<ServerDataModel>(config); auth=std::make_unique<Authenticator>((base/"no-credentials").string());
        server=std::make_unique<SentinelStreamServer>(*model,*auth,config,0); server->attachFlowChunks(*store);
        session=FlowChunkWireTest::start(*server,*model,[&](std::string payload,bool isBinary) {
            std::lock_guard lock(mutex);
            if (isBinary) binary.push_back(wire::decodeEnvelope({reinterpret_cast<const uint8_t*>(payload.data()),payload.size()}));
            else text.push_back(Json::parse(payload));
            cv.notify_all();
        });
    }
    void TearDown() override { if(session) session->stop(); if(server) server->stop(); session.reset(); }
    Json request(uint64_t req, std::vector<int64_t> starts={Epoch}) {
        return {{"req",req},{"instrument",Id},{"channel","flow.rows"},{"level_ms",Minute},{"price_step",1},{"starts",starts}};
    }
    bool wait(size_t n, bool json=false) {
        std::unique_lock lock(mutex); return cv.wait_for(lock,5s,[&] { return (json?text.size():binary.size())>=n; });
    }
};
TEST_F(FlowChunkWire, OrderedRepliesIncludeBudgetRefusalsAndRunOffExecutor) {
    std::promise<void> entered, release; auto gate=release.get_future().share();
    std::atomic<bool> once=false;
    FlowChunkWireTest::after(*server,[&](const auto& key) { if (key.startMs==Epoch && !once.exchange(true)) { entered.set_value(); gate.wait(); } });
    auto cleanup=qScopeGuard([&] { release.set_value(); FlowChunkWireTest::drain(*server,*session); });
    std::vector<int64_t> starts; for (int n=0;n<7;++n) starts.push_back(Epoch+n*Hour);
    FlowChunkWireTest::request(*session,request(7,starts));
    ASSERT_EQ(entered.get_future().wait_for(5s),std::future_status::ready);
    { std::lock_guard lock(mutex); EXPECT_TRUE(binary.empty()); }
    // A separately refused request does not wait behind another request's worker.
    FlowChunkWireTest::budget(*session,4,0); FlowChunkWireTest::request(*session,request(8));
    ASSERT_TRUE(wait(1)); EXPECT_EQ(binary[0].requestId,8u);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).error,wire::Error::Busy);
    cleanup.dismiss(); release.set_value(); ASSERT_TRUE(wait(8));
    for(size_t n=0;n<7;++n) {
        const auto& f=std::get<wire::Frame>(binary[n+1].frame);
        EXPECT_EQ(binary[n+1].requestId,7u); EXPECT_EQ(f.key.startMs,starts[n]);
        if(n>=4) EXPECT_EQ(f.error,wire::Error::Busy);
    }
    FlowChunkWireTest::on(*session,[&](auto&) { EXPECT_NE(std::this_thread::get_id(),readThread); });
    EXPECT_NE(std::this_thread::get_id(),readThread);
}
TEST_F(FlowChunkWire, ByteAndGlobalHistoryBudgetsRefuseExplicitly) {
    FlowChunkWireTest::budget(*session,0,32u*1024u*1024u);
    FlowChunkWireTest::request(*session,request(1)); ASSERT_TRUE(wait(1));
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).error,wire::Error::Busy);
    FlowChunkWireTest::budget(*session,0,0);
    std::promise<void> release; auto gate=release.get_future().share();
    auto cleanup=qScopeGuard([&] { release.set_value(); FlowChunkWireTest::drain(*server,*session); });
    for(int n=0;n<8;++n) ASSERT_TRUE(FlowChunkWireTest::submit(*server,[gate] { gate.wait(); }));
    FlowChunkWireTest::request(*session,request(2)); ASSERT_TRUE(wait(2));
    EXPECT_EQ(std::get<wire::Frame>(binary[1].frame).error,wire::Error::Busy);
}
TEST_F(FlowChunkWire, EpochChangeAfterHistoryBuildRetriesCurrentReply) {
    std::promise<void> entered, release; auto gate=release.get_future().share(); std::atomic<bool> once=false;
    FlowChunkWireTest::after(*server,[&](const auto&) { if(!once.exchange(true)) { entered.set_value(); gate.wait(); } });
    auto cleanup=qScopeGuard([&] { release.set_value(); FlowChunkWireTest::drain(*server,*session); });
    FlowChunkWireTest::request(*session,request(1)); ASSERT_EQ(entered.get_future().wait_for(5s),std::future_status::ready);
    const auto victim=source->archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(victim,fs::last_write_time(victim)+2s); ASSERT_EQ(store->runPass().failed,0u);
    cleanup.dismiss(); release.set_value(); FlowChunkWireTest::drain(*server,*session);
    ASSERT_TRUE(wait(1));
    EXPECT_EQ(binary[0].requestId, 1u);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).kind, wire::Kind::Chunk);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).epoch, store->epoch(Id));
    FlowChunkWireTest::request(*session,request(2)); ASSERT_TRUE(wait(2)); EXPECT_EQ(binary[1].requestId,2u);
}
TEST_F(FlowChunkWire, SameEpochRepublishAfterHistoryBuildRetriesCurrentReply) {
    std::promise<void> entered, release; auto gate=release.get_future().share(); std::atomic<bool> once=false;
    FlowChunkWireTest::after(*server,[&](const auto&) { if(!once.exchange(true)) { entered.set_value(); gate.wait(); } });
    auto cleanup=qScopeGuard([&] { release.set_value(); FlowChunkWireTest::drain(*server,*session); });
    const auto epoch=store->epoch(Id);
    FlowChunkWireTest::request(*session,request(1));
    ASSERT_EQ(entered.get_future().wait_for(5s),std::future_status::ready);
    Journal later(raw); later.snapshot(Hour+20000);
    later.trades(Hour+21000,{trade(Hour+100,"2",100001)});
    later.heartbeat(2*Hour+15000); later.close();
    ASSERT_EQ(store->runPass().failed,0u); ASSERT_EQ(store->epoch(Id),epoch);
    cleanup.dismiss(); release.set_value(); ASSERT_TRUE(wait(1));
    EXPECT_EQ(binary[0].requestId,1u);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).kind,wire::Kind::Chunk);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).epoch,epoch);
}
TEST_F(FlowChunkWire, FencedInFlightStartReceivesUnavailable) {
    std::promise<void> served, releaseServe, fenced, releaseFence;
    auto serveGate=releaseServe.get_future().share(), fenceGate=releaseFence.get_future().share();
    std::atomic<bool> once=false, fenceOnce=false, serveOpen=false, fenceOpen=false;
    FlowChunkWireTest::after(*server,[&](const auto&) { if(!once.exchange(true)) { served.set_value(); serveGate.wait(); } });
    beforeEpochWrite=[&] { if(!fenceOnce.exchange(true)) { fenced.set_value(); fenceGate.wait(); } };
    std::thread transition;
    auto cleanup=qScopeGuard([&] {
        if(!serveOpen.exchange(true)) releaseServe.set_value();
        if(!fenceOpen.exchange(true)) releaseFence.set_value();
        if(transition.joinable()) transition.join();
        FlowChunkWireTest::drain(*server,*session);
    });
    FlowChunkWireTest::request(*session,request(1));
    ASSERT_EQ(served.get_future().wait_for(5s),std::future_status::ready);
    const auto victim=raw/"BTC-USD"/"2027"/"01"/"01"/"00.rawl2";
    fs::last_write_time(victim,fs::last_write_time(victim)+2s);
    transition=std::thread([&] { store->runPass(); });
    ASSERT_EQ(fenced.get_future().wait_for(5s),std::future_status::ready);
    if(!serveOpen.exchange(true)) releaseServe.set_value();
    ASSERT_TRUE(wait(1));
    EXPECT_EQ(binary[0].requestId,1u);
    EXPECT_EQ(std::get<wire::Frame>(binary[0].frame).error,wire::Error::Unavailable);
}
TEST_F(FlowChunkWire, SubscribeWithoutFlowStoreReturnsError) {
    FlowChunkWireTest::on(*session,[&](auto&) { FlowChunkWireTest::detach(*server); });
    FlowChunkWireTest::subscribe(*session,{{"instruments",{Id}}}); ASSERT_TRUE(wait(1,true));
    EXPECT_EQ(text[0]["context"],"chunk_availability_subscribe");
    EXPECT_EQ(text[0]["type"],"error");
}
TEST_F(FlowChunkWire, AvailabilitySubscribeNewSealEpochAndCoalescing) {
    source->setLiveRetentionFloor("BTC-USD",Epoch-6*Hour);
    FlowChunkWireTest::subscribe(*session,{{"instruments",{Id}}}); ASSERT_TRUE(wait(1,true));
    EXPECT_EQ(text[0]["type"],"chunk_availability"); EXPECT_EQ(text[0]["live_from_ms"],Epoch-6*Hour);
    const auto initialEpoch=text[0]["channels"]["flow.rows"]["epoch"];
    Journal later(raw); later.snapshot(Hour+20000); later.trades(Hour+21000,{trade(Hour+100,"2",100001)});
    later.heartbeat(2*Hour+15000); later.close(); ASSERT_EQ(store->runPass().failed,0u);
    for(int n=0;n<10;++n) FlowChunkWireTest::poll(*session);
    FlowChunkWireTest::drain(*server,*session); EXPECT_EQ(text.size(),1u);
    FlowChunkWireTest::poll(*session,true); ASSERT_TRUE(wait(2,true));
    const auto runs=text[1]["channels"]["flow.rows"]["levels"][0]["sealed"];
    EXPECT_TRUE(std::any_of(runs.begin(),runs.end(),[](const auto& r) { return r[0]<=Epoch && r[1]>=Epoch+2*Hour; }));
    const auto victim=source->archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(victim,fs::last_write_time(victim)+2s); ASSERT_EQ(store->runPass().failed,0u);
    FlowChunkWireTest::poll(*session,true); ASSERT_TRUE(wait(3,true));
    EXPECT_NE(text[2]["channels"]["flow.rows"]["epoch"],initialEpoch);
    FlowChunkWireTest::poll(*session,true); FlowChunkWireTest::drain(*server,*session); EXPECT_EQ(text.size(),3u);
    FlowChunkWireTest::subscribe(*session,{{"instruments",Json::array()}});
    FlowChunkWireTest::poll(*session,true); FlowChunkWireTest::drain(*server,*session); EXPECT_EQ(text.size(),3u);
}
}

namespace {
class FlowChunkEndToEnd : public FlowChunkWire {
    int64_t fixtureThrough() const override {
        return heatmap::chunkStartOf(heatmap::kDayMs,Epoch)+16*heatmap::kDayMs;
    }
    bool fillDays() const override { return true; }
};
TEST_F(FlowChunkEndToEnd, ClientFetcherDecodesBothChannelsAtEveryStoredLevel) {
    namespace fc = sentinel::flow;
    namespace codec = sentinel::flow::chunks;
    SentinelStreamClient client("127.0.0.1","1");
    protocol::chunks::ClientTransport transport(client);
    fc::FlowChunkCache cache;
    fc::FlowChunkFetcher fetch(cache,transport);
    FlowChunkWireTest::sink(*session,[&](std::string bytes,bool binary) { FlowChunkClientTest::receive(client,bytes,binary); });
    auto cleanup=qScopeGuard([&] { FlowChunkWireTest::drain(*server,*session);
        FlowChunkWireTest::sink(*session,[](auto,bool) {}); });
    client.connected();
    for (int turn=0;turn<10;++turn) QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
    // Fetch each channel independently, so a cached rows body cannot mask a
    // missing summary wire path. Several sealed days/hours come from RAWL2.
    for (const auto* channel : {"flow.summary","flow.rows"}) for (auto level : heatmap::kChunkLevels) {
        fetch.hostChanged();
        const auto start=heatmap::chunkStartOf(level,Epoch);
        ASSERT_TRUE(fetch.want(1,{Id,channel,level,start,start+heatmap::chunkSpanMs(level),0}));
        const auto until=std::chrono::steady_clock::now()+10s;
        bool subscription=true;
        const protocol::chunks::Key wanted{Id,channel,level,std::string_view(channel)=="flow.rows"?1u:0u,start};
        do {
            for (int turn=0;turn<10;++turn) QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
            for (const auto& j : FlowChunkClientTest::writes(client,subscription)) {
                if (j.at("type")=="chunk_request") FlowChunkWireTest::request(*session,j);
                else FlowChunkWireTest::subscribe(*session,j);
            }
            subscription=false;
            QCoreApplication::processEvents();
            for (int turn=0;turn<10;++turn) QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
            std::this_thread::yield();
        } while ((!cache.read(wanted) || fetch.stats().inFlightChunks) && std::chrono::steady_clock::now()<until);
        ASSERT_TRUE(cache.read(wanted)) << channel << " level=" << level;
        // Check every delivered chunk, including padding, against store bytes.
        const auto snapshot=fetch.availability(Id); ASSERT_TRUE(snapshot);
        const auto plan=fc::planFlowChunks({Id,channel,level,start,start+heatmap::chunkSpanMs(level),0},*snapshot,fc::flowChunkEstimate);
        ASSERT_FALSE(plan.empty());
        for (const auto& item : plan) {
            const auto held=cache.read(item.key); ASSERT_TRUE(held);
            auto payload=store->read(Id,item.key.levelMs,item.key.startMs); ASSERT_TRUE(payload);
            auto storedKey=item.key;
            storedKey.priceStep=storedKey.channel=="flow.rows" ? uint64_t(1)<<payload->stepExponent : 0;
            const auto bytes=std::span(payload->bytes).first(storedKey.channel=="flow.rows" ? payload->bytes.size() : payload->summaryLength);
            auto expected=codec::decode(bytes,storedKey,uint16_t(heatmap::chunkSpanMs(storedKey.levelMs)/storedKey.levelMs),payload->epoch);
            EXPECT_EQ(codec::toData(*held),codec::toData(expected)); EXPECT_EQ(held->epoch,expected.epoch);
        }
        fetch.release(1);
    }
    EXPECT_EQ(client.chunkRejectedCount(),0u);
}
}
