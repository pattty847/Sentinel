// Real request handler, history pool, admission and executor/write queue; only
// the final socket write is replaced. Synthetic roots, no listener or services.
#define SENTINEL_FLOW_CHUNK_TEST
#include "protocol/SentinelStreamServer.cpp"
#include "roller/fixtures/FlowJournal.hpp"
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
    static void drain(SentinelStreamServer& s, Session& session) {
        const auto until = std::chrono::steady_clock::now()+5s;
        while (s.m_pendingHistoryTasks.load() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        EXPECT_EQ(s.m_pendingHistoryTasks.load(), 0u); on(session, [](auto&) {});
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
    void SetUp() override {
        fs::create_directories(raw); fs::create_directories(hmc);
        Journal j(raw); j.snapshot(-Minute); j.trades(1000, {trade(100,"1",100000)}); j.heartbeat(Hour+15000); j.close();
        recording::FlowStore::Options o; o.root=base/"fld"; o.hmc2Root=hmc; o.journalRoot=raw;
        o.hourThrough=[](const std::string&) { return Epoch+Day; };
        o.symbols={"BTC-USD"}; o.lowerPriority=false; o.nowMs=[] { return Epoch+Day; };
        source=std::make_unique<recording::FlowStore>(o);
        recording::FlowChunkStore::Options c; c.root=base/"flc"; c.lowerPriority=false; c.rules.sealWindowMs=15000;
        c.afterSectionReadForTest=[&] { readThread=std::this_thread::get_id(); };
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
TEST_F(FlowChunkWire, FenceAfterHistoryBuildDiscardsReplyAtSocketAdmission) {
    std::promise<void> entered, release; auto gate=release.get_future().share(); std::atomic<bool> once=false;
    FlowChunkWireTest::after(*server,[&](const auto&) { if(!once.exchange(true)) { entered.set_value(); gate.wait(); } });
    auto cleanup=qScopeGuard([&] { release.set_value(); FlowChunkWireTest::drain(*server,*session); });
    FlowChunkWireTest::request(*session,request(1)); ASSERT_EQ(entered.get_future().wait_for(5s),std::future_status::ready);
    const auto victim=source->archive("BTC-USD").catalog->files().front().path;
    fs::last_write_time(victim,fs::last_write_time(victim)+2s); ASSERT_EQ(store->runPass().failed,0u);
    cleanup.dismiss(); release.set_value(); FlowChunkWireTest::drain(*server,*session);
    { std::lock_guard lock(mutex); EXPECT_TRUE(binary.empty()); }
    FlowChunkWireTest::request(*session,request(2)); ASSERT_TRUE(wait(1)); EXPECT_EQ(binary[0].requestId,2u);
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
