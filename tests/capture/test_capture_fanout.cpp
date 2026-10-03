#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QLocalSocket>
#include <QDirIterator>
#include <QFile>
#include <thread>
#include <sys/stat.h>
#include <sys/resource.h>
#include <iostream>
using namespace sentinel::capture;
using Json = nlohmann::json;
using namespace std::chrono_literals;
namespace {
uint32_t u32(const char* p) { uint32_t n=0; for(int i=0;i<4;++i) n |= uint32_t(uint8_t(p[i])) << (8*i); return n; }
std::string framed(const Record& r) {
    std::string s;
    auto put = [&](uint64_t v, int n) { for(int i=0;i<n;++i) s += char(v >> (i*8)); };
    put(28+r.payload.size(),4); put(uint32_t(r.kind),4); put(r.time.systemNs,8); put(r.time.steadyNs,8); put(r.connection,8); s+=r.payload; return s;
}
struct Peer {
    QLocalSocket socket;
    QByteArray buffer;
    explicit Peer(const QString& path) { socket.connectToServer(path); if(!socket.waitForConnected(3000)) throw std::runtime_error("connect failed"); }
    void send(Json j) { const auto s=j.dump()+"\n"; socket.write(s.data(),s.size()); socket.waitForBytesWritten(1000); }
    void hello(std::optional<JournalPosition> p={}, std::string product="BTC-USD") {
        Json j={{"type","hello"},{"version",1},{"product",product}}; if(p) j["pos"]=positionJson(*p); send(j);
    }
    std::pair<Json,std::string> next(int ms=3000) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
        while(true) {
            buffer+=socket.readAll();
            if(buffer.size()>=4) {
                const auto size=u32(buffer.data());
                if(size>MaxRecordBytes+4096 || size<4) throw std::runtime_error("bad length");
                if(buffer.size()>=qsizetype(size+4)) {
                    const auto jsonSize=u32(buffer.data()+4);
                    if(jsonSize>size-4) throw std::runtime_error("bad json length");
                    auto j=Json::parse(buffer.data()+8,buffer.data()+8+jsonSize);
                    std::string raw(buffer.data()+8+jsonSize,size-4-jsonSize); buffer.remove(0,size+4); return {j,raw};
                }
            }
            if(std::chrono::steady_clock::now()>=until || socket.state()==QLocalSocket::UnconnectedState) throw std::runtime_error("read timeout/EOF");
            socket.waitForReadyRead(10);
        }
    }
};
bool eventually(auto f) {
    auto until=std::chrono::steady_clock::now()+3s;
    while(std::chrono::steady_clock::now()<until) { if(f()) return true; std::this_thread::sleep_for(1ms); } return f();
}
struct Fixture : testing::Test {
    QTemporaryDir dir{"/tmp/sf-XXXXXX"};
    sentinel::metrics::MetricsRegistry metrics;
    FanoutConfig cfg;
    std::atomic<int64_t> now{1000000000};
    std::mutex requestsMutex;
    std::vector<std::string> requests;
    std::unique_ptr<CaptureFanout> server;
    void start(std::vector<std::string> products={"BTC-USD"}) {
        static int argc = 1; static char name[] = "test_capture_fanout"; static char* argv[] = {name, nullptr};
        if (!QCoreApplication::instance()) { static QCoreApplication app(argc, argv); }
        ASSERT_TRUE(dir.isValid()); cfg.socketPath=dir.path()+"/f.sock"; cfg.nowNs=[&]{return now.load();};
        server=std::make_unique<CaptureFanout>(cfg,products,metrics,[&](const auto& p){std::lock_guard lock(requestsMutex);requests.push_back(p);});
    }
    void put(uint64_t block, std::string payload="raw", size_t product=0) {
        Record r{Kind::Frame,{1700000000000000000LL+int64_t(block),int64_t(block)+1},7,std::move(payload)};
        server->publish(product,"run",block,1,framed(r));
    }
    bool metric(const std::string& line) { return metrics.render().find(line+"\n")!=std::string::npos; }
};
TEST_F(Fixture, WriterBytesPositionsRotationAndShutdown) {
    start(); Peer peer(server->path()); peer.hello(); ASSERT_EQ(peer.next().first["type"],"tip");
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockBytes=160; wc.fsyncBlocks=1;
    wc.onBlock=[&](auto run,auto block,auto count,auto bytes){server->publish(0,run,block,count,bytes);};
    std::vector<Record> expected;
    {
        Session session(wc,{{"product_metadata",{{"product_id","BTC-USD"},{"quote_increment","0.01"},{"base_increment","0.00000001"}}}});
        for(int i=0;i<8;++i) {
            Record r{Kind(i+1),{1700000000000000000LL+int64_t(i/4)*3600000000000LL,i+1},2," {\"exact\": \"00.100\"} \n"};
            if(r.kind==Kind::CaptureStopped) continue; // session owns terminal record
            expected.push_back(r); ASSERT_TRUE(session.submit(r));
        }
        Record stop{Kind::CaptureStopped,{1700003600000000010LL,10},2,"{}"}; expected.push_back(stop);
        ASSERT_TRUE(session.submit(stop)); session.close(); ASSERT_TRUE(session.error().empty());
    }
    std::vector<JournalPosition> positions;
    for(const auto& r:expected) {
        auto [j,raw]=peer.next(); ASSERT_EQ(j["type"],"record"); EXPECT_EQ(raw,framed(r)); positions.push_back(parsePosition(j["pos"]));
    }
    std::map<uint64_t,std::vector<Record>> blocks;
    QDirIterator files(wc.root,{"*.rawl2"},QDir::Files,QDirIterator::Subdirectories);
    std::string run;
    while(files.hasNext()) {
        std::vector<Record> records; auto result=scan(files.next(),[&](auto& r){records.push_back(r);});
        run=result.header["run_id"]; size_t n=0;
        for(const auto& b:result.index) for(uint32_t i=0;i<b.records;++i) blocks[b.ordinal].push_back(records[n++]);
    }
    ASSERT_EQ(positions.size(),expected.size());
    for(size_t i=0;i<positions.size();++i) {
        EXPECT_EQ(positions[i].product,"BTC-USD"); EXPECT_EQ(positions[i].runId,run);
        EXPECT_EQ(blocks.at(positions[i].block).at(positions[i].record),expected[i]);
        if(i) EXPECT_TRUE(positions[i].block>positions[i-1].block || (positions[i].block==positions[i-1].block && positions[i].record==positions[i-1].record+1));
    }
    server->stop(); EXPECT_EQ(peer.next().first["reason"],"shutdown");
    EXPECT_TRUE(metric("sentinel_fanout_clients 0")); server.reset(); EXPECT_FALSE(QFileInfo::exists(cfg.socketPath));
}
TEST_F(Fixture, ResumeExcludesCursorAndIncludesEverySuccessor) {
    start(); Peer first(server->path()); first.hello(); first.next();
    for(int i=0;i<4;++i) { put(i); auto r=first.next(); EXPECT_EQ(r.first["pos"]["block"],i); }
    Peer resumed(server->path()); resumed.hello(JournalPosition{"BTC-USD","run",1,0});
    EXPECT_EQ(resumed.next().first["type"],"tip");
    for(int i=2;i<4;++i) EXPECT_EQ(resumed.next().first["pos"]["block"],i);
    put(4); EXPECT_EQ(resumed.next().first["pos"]["block"],4);
    EXPECT_TRUE(metric("sentinel_fanout_resume_hits_total{product=\"BTC-USD\"} 1"));
}
TEST_F(Fixture, TimeAndByteEvictionReturnExplicitJournalBoundary) {
    cfg.ringBytes=400; start(); Peer live(server->path()); live.hello(); live.next();
    put(0); live.next(); put(1); live.next(); put(2); live.next();
    Peer bytes(server->path()); bytes.hello(JournalPosition{"BTC-USD","run",0,0});
    auto gap=bytes.next().first; ASSERT_EQ(gap["type"],"gap"); EXPECT_EQ(gap["journal_until"]["block"],2); EXPECT_FALSE(gap["until_inclusive"].get<bool>());
    bytes.next(); EXPECT_EQ(bytes.next().first["pos"]["block"],2);
    now+=60000000001LL;
    Peer age(server->path()); age.hello(JournalPosition{"BTC-USD","run",2,0});
    gap=age.next().first; EXPECT_EQ(gap["type"],"gap"); EXPECT_TRUE(gap["journal_until"].is_null());
    age.next(); put(3); live.next(); EXPECT_EQ(age.next().first["pos"]["block"],3);
    Peer foreign(server->path()); foreign.hello(JournalPosition{"BTC-USD","previous-run",3,0});
    EXPECT_EQ(foreign.next().first["type"],"gap");
    EXPECT_TRUE(metric("sentinel_fanout_resume_misses_total{product=\"BTC-USD\"} 3"));
}
TEST_F(Fixture, SlowClientCannotBlockWriterOrHealthyClient) {
    cfg.clientBytes=256*1024; start();
    Peer slow(server->path()); slow.hello(); slow.next();
    Peer healthy(server->path()); healthy.hello(); healthy.next();
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockBytes=1; wc.fsyncBlocks=0;
    wc.onBlock=[&](auto run,auto block,auto count,auto bytes){server->publish(0,run,block,count,bytes);};
    Writer writer(wc,Json::object());
    auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<40;++i) {
        writer.append({Kind::Frame,{1700000000000000000LL+i,i+1},1,std::string(65536,'x')});
        auto r=healthy.next(); ASSERT_EQ(r.first["type"],"record"); EXPECT_EQ(r.first["pos"]["block"],i);
    }
    writer.close(); EXPECT_EQ(writer.stats().records,40);
    EXPECT_LT(std::chrono::steady_clock::now()-begin,3s);
    EXPECT_TRUE(eventually([&]{return metric("sentinel_fanout_disconnects_total{reason=\"slow_client\"} 1");}));
    EXPECT_TRUE(metric("sentinel_fanout_clients 1"));
}
TEST_F(Fixture, IngressOverflowInvalidatesResumeWithoutFailingWriter) {
    cfg.ingressBytes=1024; start(); Peer peer(server->path()); peer.hello(); peer.next();
    put(0); peer.next(); put(1,std::string(2048,'x'));
    EXPECT_EQ(peer.next().first["reason"],"ingress_overflow");
    put(2);
    Peer recovered(server->path()); recovered.hello(JournalPosition{"BTC-USD","run",0,0});
    EXPECT_EQ(recovered.next().first["type"],"gap");
    EXPECT_TRUE(metric("sentinel_fanout_disconnects_total{reason=\"ingress_overflow\"} 1"));
}
TEST_F(Fixture, ResnapshotRoutesAndRateLimitsAcrossClientsPerProduct) {
    start({"BTC-USD","ETH-USD"}); Peer a(server->path()), b(server->path()), eth(server->path());
    a.hello(); a.next(); b.hello(); b.next(); eth.hello({},"ETH-USD"); eth.next();
    auto request=[](Peer& p,std::string product="BTC-USD") {p.send({{"type","resnapshot"},{"product",product}});return p.next().first["status"];};
    EXPECT_EQ(request(a),"accepted"); EXPECT_EQ(request(b),"rate_limited"); EXPECT_EQ(request(eth,"ETH-USD"),"accepted");
    now+=10000000000LL; EXPECT_EQ(request(b),"accepted");
    std::lock_guard lock(requestsMutex); EXPECT_EQ(requests,(std::vector<std::string>{"BTC-USD","ETH-USD","BTC-USD"}));
}
TEST_F(Fixture, SocketSafetyOwnershipAndFailedStartupNeverDeletesFiles) {
    EXPECT_THROW(prepareFanoutPath("relative.sock"),std::exception);
    EXPECT_THROW(prepareFanoutPath("/Volumes/T7/must-not-create/f.sock"),std::exception);
    EXPECT_THROW(prepareFanoutPath(QString(SENTINEL_SOURCE_ROOT)+"/must-not-create/f.sock"),std::exception);
    QDir().mkdir(dir.path()+"/public"); ::chmod((dir.path()+"/public").toStdString().c_str(),0755);
    EXPECT_THROW(prepareFanoutPath(dir.path()+"/public/f.sock"),std::exception);
    ASSERT_TRUE(QFile::link(QString(SENTINEL_SOURCE_ROOT),dir.path()+"/repo"));
    EXPECT_THROW(prepareFanoutPath(dir.path()+"/repo/f.sock"),std::exception);
    start(); struct stat st{}; ASSERT_EQ(::stat(dir.path().toStdString().c_str(),&st),0); EXPECT_EQ(st.st_mode&0777,0700);
    sentinel::metrics::MetricsRegistry other;
    EXPECT_THROW(CaptureFanout(cfg,{"BTC-USD"},other,{}),std::exception);
    Peer still(server->path()); still.hello(); EXPECT_EQ(still.next().first["type"],"tip");
    server.reset(); QFile file(cfg.socketPath); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("preserve"); file.close();
    sentinel::metrics::MetricsRegistry third;
    EXPECT_THROW(CaptureFanout(cfg,{"BTC-USD"},third,{}),std::exception); ASSERT_TRUE(file.open(QIODevice::ReadOnly)); EXPECT_EQ(file.readAll(),"preserve");
}
TEST_F(Fixture, MalformedOversizedUnknownAndIdleClientsAreBounded) {
    start(); Peer malformed(server->path()); malformed.send({{"type","hello"},{"version",1},{"product","NOPE-USD"}});
    EXPECT_EQ(malformed.next().first["reason"],"protocol");
    Peer huge(server->path()); huge.socket.write(QByteArray(10000,'x')); huge.socket.waitForBytesWritten(); EXPECT_EQ(huge.next().first["reason"],"protocol");
    Peer idle(server->path()); ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_clients 1");})); now+=6000000000LL;
    EXPECT_EQ(idle.next().first["reason"],"handshake_timeout");
}
TEST_F(Fixture, IdleAllocatesNoPayloadAndRetentionIsIndependentOfClients) {
    start(); EXPECT_TRUE(metric("sentinel_fanout_clients 0"));
    EXPECT_TRUE(metric("sentinel_fanout_ring_bytes{product=\"BTC-USD\"} 0"));
    EXPECT_TRUE(metric("sentinel_fanout_ingress_bytes{product=\"BTC-USD\"} 0"));
    put(0); ASSERT_TRUE(eventually([&]{return !metric("sentinel_fanout_ring_bytes{product=\"BTC-USD\"} 0");}));
    Peer peer(server->path()); peer.hello(JournalPosition{"BTC-USD","run",0,0}); EXPECT_EQ(peer.next().first["type"],"tip");
    EXPECT_TRUE(metric("sentinel_fanout_resume_hits_total{product=\"BTC-USD\"} 1"));
}
TEST_F(Fixture, ClientCapAndUnavailableControlHaveBoundedLifetimes) {
    std::atomic<bool> controlReady{false}; cfg.controlReady=[&]{return controlReady.load();}; start();
    std::vector<std::unique_ptr<Peer>> peers;
    for(size_t i=0;i<CaptureFanout::MaxClients;++i) { auto p=std::make_unique<Peer>(server->path()); p->hello(); EXPECT_EQ(p->next().first["type"],"tip"); peers.push_back(std::move(p)); }
    Peer extra(server->path()); EXPECT_EQ(extra.next().first["reason"],"capacity");
    peers[0]->send({{"type","resnapshot"},{"product","BTC-USD"}}); EXPECT_EQ(peers[0]->next().first["status"],"unavailable");
    controlReady=true;
    peers[0]->send({{"type","resnapshot"},{"product","BTC-USD"}}); EXPECT_EQ(peers[0]->next().first["status"],"accepted");
    auto begin=std::chrono::steady_clock::now(); server->stop(); EXPECT_LT(std::chrono::steady_clock::now()-begin,1s);
    for(auto& p:peers) EXPECT_EQ(p->next().first["reason"],"shutdown");
}
TEST_F(Fixture, FailedOrUnflushedBlocksAreNotPublished) {
    start(); Peer peer(server->path()); peer.hello(); peer.next();
    WriterConfig wc; wc.root=dir.path()+"/raw";
    std::atomic<int> calls{0};
    wc.onBlock=[&](auto run,auto block,auto count,auto bytes){++calls; server->publish(0,run,block,count,bytes);};
    Writer writer(wc,Json::object()); writer.append({Kind::Frame,Stamp::now(),1,"first"});
    EXPECT_EQ(calls,0); writer.flush(); EXPECT_EQ(calls,1); EXPECT_EQ(peer.next().first["type"],"record");
    writer.append({Kind::Frame,Stamp::now(),1,"abandoned"}); writer.abandonSegment(); writer.close();
    EXPECT_EQ(calls,1);
}
} // namespace
