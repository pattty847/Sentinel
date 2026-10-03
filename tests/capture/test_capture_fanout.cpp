#include "capture/CaptureFanout.hpp"
#include "capture/CaptureSession.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QLocalSocket>
#include <QDirIterator>
#include <QFile>
#include <thread>
#include <sys/stat.h>
#include <sys/resource.h>
#include <iostream>
#include <csignal>
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
    std::pair<Json,std::string> nextNonDurable() {
        auto event = next(); while (event.first["type"] == "durable") event = next(); return event;
    }
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
    std::atomic<unsigned> clockReads{0};
    std::mutex requestsMutex;
    std::vector<std::string> requests;
    std::unique_ptr<CaptureFanout> server;
    void start(std::vector<std::string> products={"BTC-USD"}) {
        static int argc = 1; static char name[] = "test_capture_fanout"; static char* argv[] = {name, nullptr};
        if (!QCoreApplication::instance()) { static QCoreApplication app(argc, argv); }
        ASSERT_TRUE(dir.isValid()); if(cfg.socketPath.isEmpty()) cfg.socketPath=dir.path()+"/f.sock"; cfg.nowNs=[&]{++clockReads;return now.load();};
        server=std::make_unique<CaptureFanout>(cfg,products,metrics,[&](const auto& p){std::lock_guard lock(requestsMutex);requests.push_back(p);});
    }
    void put(uint64_t block, std::string payload="raw", size_t product=0) {
        Record r{Kind::Frame,{1700000000000000000LL+int64_t(block),int64_t(block)+1},7,std::move(payload)};
        server->publish(product,{JournalEventKind::Record,"run",block,0,true,framed(r)});
    }
    bool metric(const std::string& line) { return metrics.render().find(line+"\n")!=std::string::npos; }
};
TEST_F(Fixture, WriterBytesPositionsRotationAndShutdown) {
    start(); Peer peer(server->path()); peer.hello(); ASSERT_EQ(peer.next().first["type"],"tip");
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockBytes=160; wc.fsyncBlocks=1;
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
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
        auto [j,raw]=peer.nextNonDurable(); ASSERT_EQ(j["type"],"record"); EXPECT_EQ(raw,framed(r)); positions.push_back(parsePosition(j["pos"]));
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
    server->stop(); EXPECT_EQ(peer.nextNonDurable().first["reason"],"shutdown");
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
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    Writer writer(wc,Json::object());
    auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<40;++i) {
        writer.append({Kind::Frame,{1700000000000000000LL+i,i+1},1,std::string(65536,'x')});
        auto r=healthy.nextNonDurable(); ASSERT_EQ(r.first["type"],"record"); EXPECT_EQ(r.first["pos"]["block"],i);
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
    EXPECT_EQ(request(a),"forwarded"); EXPECT_EQ(request(b),"rate_limited"); EXPECT_EQ(request(eth,"ETH-USD"),"forwarded");
    now+=20000000000LL; EXPECT_EQ(request(b),"forwarded");
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
    { CaptureFanout blocked(cfg,{"BTC-USD"},other,{}); EXPECT_NE(other.render().find("sentinel_fanout_running 0\n"),std::string::npos); }
    Peer still(server->path()); still.hello(); EXPECT_EQ(still.next().first["type"],"tip");
    server.reset(); QFile file(cfg.socketPath); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("preserve"); file.close();
    sentinel::metrics::MetricsRegistry third;
    CaptureFanout blocked(cfg,{"BTC-USD"},third,{});
    EXPECT_NE(third.render().find("sentinel_fanout_running 0\n"),std::string::npos); ASSERT_TRUE(file.open(QIODevice::ReadOnly)); EXPECT_EQ(file.readAll(),"preserve");
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
    peers[0]->send({{"type","resnapshot"},{"product","BTC-USD"}}); EXPECT_EQ(peers[0]->next().first["status"],"forwarded");
    auto begin=std::chrono::steady_clock::now(); server->stop(); EXPECT_LT(std::chrono::steady_clock::now()-begin,1s);
    for(auto& p:peers) EXPECT_EQ(p->next().first["reason"],"shutdown");
}
TEST_F(Fixture, SetupFailureRetriesWithBackoffWhileWriterContinues) {
    ASSERT_TRUE(QDir().mkdir(dir.path()+"/socket"));
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0755),0);
    cfg.socketPath=dir.path()+"/socket/f.sock"; start();
    EXPECT_TRUE(metric("sentinel_fanout_running 0")); EXPECT_TRUE(metric("sentinel_fanout_setup_failures_total 1"));
    WriterConfig wc; wc.root=dir.path()+"/raw";
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    Writer writer(wc,Json::object()); writer.append({Kind::Frame,Stamp::now(),1,"kept"}); writer.flush();
    EXPECT_EQ(writer.stats().blocks,1); EXPECT_GT(QFileInfo(writer.currentPath()).size(),0);
    now+=30'000'000'000LL; put(10);
    ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_setup_failures_total 2");}));
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0700),0);
    now+=59'000'000'000LL; const auto reads=clockReads.load(); put(11); // second delay is 60 s
    // Two worker turns at the advanced fake clock prove it checked the deadline.
    ASSERT_TRUE(eventually([&]{return clockReads.load()>=reads+6;}));
    EXPECT_TRUE(metric("sentinel_fanout_running 0"));
    now+=1'000'000'000LL; put(12);
    ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_running 1");}));
    Peer peer(server->path()); peer.hello(); EXPECT_EQ(peer.next().first["type"],"tip");
    EXPECT_TRUE(metric("sentinel_fanout_setup_failures_total 2"));
    writer.close();
}
TEST_F(Fixture, SetupRetryPreservesRingForExclusiveResume) {
    ASSERT_TRUE(QDir().mkdir(dir.path()+"/socket"));
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0755),0);
    cfg.socketPath=dir.path()+"/socket/f.sock";
    cfg.retention=5min; // Keep the original cursor beyond the 30 s + 60 s retries.
    start();
    ASSERT_TRUE(metric("sentinel_fanout_setup_failures_total 1"));
    put(0);
    ASSERT_TRUE(eventually([&]{return !metric("sentinel_fanout_ring_bytes{product=\"BTC-USD\"} 0");}));

    now+=30'000'000'000LL;
    ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_setup_failures_total 2");}));
    EXPECT_FALSE(metric("sentinel_fanout_ring_bytes{product=\"BTC-USD\"} 0"));
    put(1);
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0700),0);
    now+=60'000'000'000LL;
    ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_running 1");}));

    Peer peer(server->path());
    peer.hello(JournalPosition{"BTC-USD","run",0,0});
    EXPECT_EQ(peer.next().first["type"],"tip");
    EXPECT_EQ(peer.next().first["pos"]["block"],1);
    EXPECT_TRUE(metric("sentinel_fanout_resume_hits_total{product=\"BTC-USD\"} 1"));
}
TEST_F(Fixture, SetupRetryBackoffCapsAtTenMinutesAndStopsWhileDown) {
    ASSERT_TRUE(QDir().mkdir(dir.path()+"/socket"));
    ASSERT_EQ(::chmod((dir.path()+"/socket").toStdString().c_str(),0755),0);
    cfg.socketPath=dir.path()+"/socket/f.sock"; start();
    int attempts=1;
    for(int seconds:{30,60,120,240,480,600,600}) {
        now+=int64_t(seconds)*1'000'000'000LL; put(attempts); ++attempts;
        ASSERT_TRUE(eventually([&]{return metric("sentinel_fanout_setup_failures_total "+std::to_string(attempts));}));
    }
    EXPECT_TRUE(metric("sentinel_fanout_running 0"));
    auto begin=std::chrono::steady_clock::now(); server->stop();
    EXPECT_LT(std::chrono::steady_clock::now()-begin,2s); EXPECT_FALSE(QFileInfo::exists(cfg.socketPath));
}
TEST_F(Fixture, ResnapshotCooldownAndGlobalCapReportForwarding) {
    start({"BTC-USD","ETH-USD","SOL-USD","XRP-USD"});
    Peer btc(server->path()),eth(server->path()),sol(server->path()),xrp(server->path());
    const auto request=[](Peer& p,const char* product) {p.send({{"type","resnapshot"},{"product",product}}); return p.next().first["status"];};
    btc.hello();btc.next();eth.hello({},"ETH-USD");eth.next();sol.hello({},"SOL-USD");sol.next();xrp.hello({},"XRP-USD");xrp.next();
    EXPECT_EQ(request(btc,"BTC-USD"),"forwarded");
    now+=10'000'000'000LL; EXPECT_EQ(request(btc,"BTC-USD"),"rate_limited");
    now+=10'000'000'000LL; EXPECT_EQ(request(btc,"BTC-USD"),"forwarded");
    EXPECT_EQ(request(eth,"ETH-USD"),"forwarded");
    EXPECT_EQ(request(sol,"SOL-USD"),"global_rate_limited");
    EXPECT_EQ(request(xrp,"XRP-USD"),"global_rate_limited");
    now+=39'000'000'000LL; EXPECT_EQ(request(sol,"SOL-USD"),"global_rate_limited");
    now+=1'000'000'000LL; EXPECT_EQ(request(sol,"SOL-USD"),"forwarded");
    EXPECT_EQ(request(xrp,"XRP-USD"),"global_rate_limited");
    std::lock_guard lock(requestsMutex); EXPECT_EQ(requests,(std::vector<std::string>{"BTC-USD","BTC-USD","ETH-USD","SOL-USD"}));
}
std::atomic<int> malformedLogs{0};
TEST_F(Fixture, MalformedIngressIsolatesOnlyItsProduct) {
    malformedLogs=0;
    struct LogScope {
        QtMessageHandler old=qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){
            if(text.contains("Capture fanout malformed ingress: product=BTC-USD")) ++malformedLogs;
        });
        ~LogScope(){qInstallMessageHandler(old);}
    } logs;
    start({"BTC-USD","ETH-USD"}); Peer btc(server->path()),eth(server->path());
    btc.hello();btc.next();eth.hello({},"ETH-USD");eth.next();
    put(0); btc.next(); put(0,"healthy",1); eth.next();
    server->publish(0,{JournalEventKind::Record,"run",1,0,true,"bad"});
    EXPECT_EQ(btc.next().first["reason"],"malformed_ingress");
    EXPECT_TRUE(metric("sentinel_fanout_running 1")); EXPECT_TRUE(metric("sentinel_fanout_clients 1"));
    EXPECT_TRUE(eventually([&]{return metric("sentinel_fanout_ring_bytes{product=\"BTC-USD\"} 0");}));
    put(1,"still healthy",1); EXPECT_EQ(eth.next().first["pos"]["block"],1);
    Peer resumed(server->path()); resumed.hello(JournalPosition{"BTC-USD","run",0,0});
    EXPECT_EQ(resumed.next().first["type"],"gap"); resumed.next();
    // Reject inconsistent length too, without poisoning other products or resume.
    Record r{Kind::Frame,Stamp::now(),1,"x"}; auto bad=framed(r); bad[0]=0;
    server->publish(0,{JournalEventKind::Record,"run",2,0,true,bad});
    EXPECT_EQ(resumed.next().first["reason"],"malformed_ingress");
    put(2,"uninterrupted",1); EXPECT_EQ(eth.next().first["pos"]["block"],2);
    EXPECT_TRUE(metric("sentinel_fanout_running 1"));
    server->stop(); EXPECT_EQ(malformedLogs.load(),1);
}
TEST_F(Fixture, ProvisionalArrivesBeforeFlushAndDurableFollows) {
    start(); Peer peer(server->path()); peer.hello(); peer.next();
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockInterval=60s;
    std::vector<JournalEventKind> events;
    wc.onJournal=[&](const JournalEvent& e){
        events.push_back(e.kind);
        QDirIterator files(wc.root,{"*.rawl2"},QDir::Files,QDirIterator::Subdirectories);
        if(e.kind==JournalEventKind::Record) EXPECT_FALSE(files.hasNext()); // before even initial header I/O
        if(e.kind==JournalEventKind::Durable) {
            ASSERT_TRUE(files.hasNext());
            auto disk=scan(files.next()); ASSERT_EQ(disk.index.size(),1);
            EXPECT_EQ(disk.index.front().ordinal,e.block); EXPECT_EQ(disk.index.front().records,e.record+1);
        }
        server->publish(0,e);
    };
    Writer writer(wc,{{"product_metadata",{{"product_id","BTC-USD"},{"quote_increment","0.01"},{"base_increment","0.00000001"}}}});
    Record record{Kind::Frame,Stamp::now(),1,"first"}; writer.append(record);
    auto provisional=peer.next(); ASSERT_EQ(provisional.first["type"],"record");
    EXPECT_TRUE(provisional.first["provisional"].get<bool>()); EXPECT_EQ(provisional.second,framed(record));
    EXPECT_EQ(writer.stats().blocks,0); EXPECT_EQ(events,(std::vector{JournalEventKind::Record}));
    const auto pos=provisional.first["pos"];
    // Reconnect while still unflushed: the cursor is in the ring, but not durable.
    Peer resumed(server->path()); resumed.hello(parsePosition(pos));
    auto tip=resumed.next().first; EXPECT_EQ(tip["type"],"tip"); EXPECT_TRUE(tip["durable"].is_null());
    writer.flush();
    auto durable=peer.next(); ASSERT_EQ(durable.first["type"],"durable");
    EXPECT_EQ(durable.first["through"],pos); EXPECT_EQ(durable.first["product"],"BTC-USD"); EXPECT_TRUE(durable.second.empty());
    EXPECT_EQ(resumed.next().first["through"],pos); EXPECT_EQ(writer.stats().blocks,1);
    EXPECT_EQ(events,(std::vector{JournalEventKind::Record,JournalEventKind::Durable}));
}
TEST_F(Fixture, DurableProvisionalOrderAndResumeWatermark) {
    start(); Peer peer(server->path()); peer.hello(); peer.next();
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockBytes=64;
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    Writer writer(wc,Json::object());
    writer.append({Kind::Frame,Stamp::now(),1,"first"});
    auto first=peer.next().first; ASSERT_EQ(first["type"],"record");
    // The next provisional record arrives before the preceding block does disk I/O.
    writer.append({Kind::Frame,Stamp::now(),1,"second"});
    auto second=peer.next().first; ASSERT_EQ(second["type"],"record"); EXPECT_TRUE(second["provisional"].get<bool>());
    auto durable=peer.next().first; ASSERT_EQ(durable["type"],"durable"); EXPECT_EQ(durable["through"],first["pos"]);
    EXPECT_EQ(second["pos"]["block"],1); EXPECT_EQ(second["pos"]["record"],0);
    writer.flush(); EXPECT_EQ(peer.next().first["through"],second["pos"]);
    Peer resumed(server->path()); resumed.hello(parsePosition(first["pos"]));
    auto tip=resumed.next().first; EXPECT_EQ(tip["durable"],second["pos"]);
    EXPECT_EQ(resumed.next().first["pos"],second["pos"]);
}
// Tiny consumer model: display provisional entries, checkpoint only an applied
// durable prefix, and pause for journal recovery immediately after retraction.
struct Consumer {
    std::vector<JournalPosition> displayed;
    std::optional<JournalPosition> checkpoint;
    bool needsJournal=false;
    void accept(const Json& j) {
        if(j["type"]=="retract") {
            std::optional<JournalPosition> after;
            if(!j["after"].is_null()) after=parsePosition(j["after"]);
            std::erase_if(displayed,[&](const auto& p){return !after || p.runId!=after->runId ||
                std::pair{p.block,p.record}>std::pair{after->block,after->record};});
            needsJournal=true;
        } else if(!needsJournal && j["type"]=="record") displayed.push_back(parsePosition(j["pos"]));
        else if(!needsJournal && j["type"]=="durable") {
            auto through=parsePosition(j["through"]);
            EXPECT_NE(std::find(displayed.begin(),displayed.end(),through),displayed.end());
            checkpoint=through;
        }
    }
};
TEST_F(Fixture, WriteFailureRetractsAndConsumerDiscardsSuffix) {
    start(); Peer peer(server->path()); peer.hello(); peer.next(); Consumer consumer;
    WriterConfig wc; wc.root=dir.path()+"/raw";
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    Writer writer(wc,Json::object());
    writer.append({Kind::Frame,Stamp::now(),1,"durable prefix"});
    const auto first=peer.next().first; consumer.accept(first); EXPECT_FALSE(consumer.checkpoint);
    writer.flush(); consumer.accept(peer.next().first); ASSERT_TRUE(consumer.checkpoint);
    const auto checkpoint=*consumer.checkpoint;
    std::string random(32768,' '); uint32_t state=123;
    for(auto& c:random) {state=state*1664525u+1013904223u; c=char(state>>24);}
    writer.append({Kind::Frame,Stamp::now(),1,random});
    auto provisional=peer.next().first; consumer.accept(provisional);
    EXPECT_EQ(consumer.displayed.size(),2); EXPECT_EQ(*consumer.checkpoint,checkpoint);
    // Real short write without filling a volume; only this scoped file-size
    // limit is changed, and both limit and signal disposition are restored.
    {
        struct Limit {
            rlimit previous{}; decltype(std::signal(SIGXFSZ,SIG_IGN)) handler;
            Limit():handler(std::signal(SIGXFSZ,SIG_IGN)) {
                if(getrlimit(RLIMIT_FSIZE,&previous)) throw std::runtime_error("getrlimit");
                auto limited=previous; limited.rlim_cur=8192;
                if(setrlimit(RLIMIT_FSIZE,&limited)) throw std::runtime_error("setrlimit");
            }
            ~Limit(){setrlimit(RLIMIT_FSIZE,&previous); std::signal(SIGXFSZ,handler);}
        } limit;
        EXPECT_THROW(writer.flush(),std::exception);
    }
    auto retract=peer.next().first; ASSERT_EQ(retract["type"],"retract"); EXPECT_EQ(retract["after"],first["pos"]);
    consumer.accept(retract); EXPECT_TRUE(consumer.needsJournal);
    ASSERT_EQ(consumer.displayed.size(),1); EXPECT_EQ(consumer.displayed.front(),checkpoint); EXPECT_EQ(*consumer.checkpoint,checkpoint);
    // Retracted records cannot be replayed from the ring.
    Peer stale(server->path()); stale.hello(parsePosition(provisional["pos"])); EXPECT_EQ(stale.next().first["type"],"gap");
    writer.abandonSegment(); writer.append({Kind::CaptureStopped,Stamp::now(),1,R"({"gap":true})"}); writer.close();
    auto marker=peer.next().first; ASSERT_EQ(marker["type"],"record");
    EXPECT_GT(marker["pos"]["block"].get<uint64_t>(),provisional["pos"]["block"].get<uint64_t>());
    consumer.accept(marker); consumer.accept(peer.next().first); EXPECT_EQ(*consumer.checkpoint,checkpoint); // paused until journal recovery
}
TEST_F(Fixture, RetractionBeforeFirstFlushIsNullAndPositionsAreNotReused) {
    start(); Peer peer(server->path()); peer.hello(); peer.next(); Consumer consumer;
    WriterConfig wc; wc.root=dir.path()+"/raw";
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    Writer writer(wc,Json::object()); writer.append({Kind::Frame,Stamp::now(),1,"abandoned"});
    auto first=peer.next().first; consumer.accept(first);
    writer.abandonSegment(); auto retract=peer.next().first;
    ASSERT_EQ(retract["type"],"retract"); EXPECT_TRUE(retract["after"].is_null()); consumer.accept(retract);
    EXPECT_TRUE(consumer.displayed.empty()); EXPECT_FALSE(consumer.checkpoint);
    Peer resumed(server->path()); resumed.hello(parsePosition(first["pos"]));
    EXPECT_EQ(resumed.next().first["type"],"gap"); EXPECT_TRUE(resumed.next().first["durable"].is_null());
    writer.append({Kind::CaptureStopped,Stamp::now(),1,R"({"gap":true})"});
    auto marker=peer.next().first; EXPECT_GT(marker["pos"]["block"].get<uint64_t>(),first["pos"]["block"].get<uint64_t>());
    writer.close(); EXPECT_EQ(peer.next().first["type"],"durable");
}
TEST_F(Fixture, SessionFaultRetractsBeforeWaitingForClose) {
    start(); Peer peer(server->path()); peer.hello(); peer.next();
    WriterConfig wc; wc.root=dir.path()+"/raw"; wc.blockInterval=60s;
    wc.onJournal=[&](const JournalEvent& e){server->publish(0,e);};
    std::atomic<bool> fail{false}; SessionHooks hooks;
    hooks.beforeWriterOperation=[&](const auto&,auto op,const auto*){if(op=="flush" && fail.exchange(false)) throw std::runtime_error("injected write failure");};
    Session session(wc,Json::object(),65536,hooks); ASSERT_TRUE(session.submit({Kind::Frame,Stamp::now(),1,"pending"}));
    EXPECT_EQ(peer.next().first["type"],"record"); fail=true;
    auto retract=peer.next().first; EXPECT_EQ(retract["type"],"retract"); EXPECT_TRUE(retract["after"].is_null());
    EXPECT_TRUE(eventually([&]{return !session.error().empty();})); session.close(); EXPECT_FALSE(session.error().empty());
}
} // namespace

TEST(CaptureFanoutAlert, UnavailableForFiveMinutesAlertsWithoutPagingOnAbsence) {
    const auto root=YAML::LoadFile(std::string(SENTINEL_SOURCE_ROOT)+"/ops/monitoring/grafana/provisioning/alerting/rules.yaml");
    YAML::Node alert;
    for(size_t g=0;g<root["groups"].size();++g) {
        const auto rules=root["groups"][g]["rules"];
        for(size_t i=0;i<rules.size();++i) {
            const auto rule=rules[i];
            if(rule["uid"].as<std::string>()=="sentinel-capture-fanout-down") alert=rule;
        }
    }
    ASSERT_TRUE(alert.IsMap());
    EXPECT_EQ(alert["for"].as<std::string>(),"5m"); EXPECT_EQ(alert["noDataState"].as<std::string>(),"OK");
    EXPECT_EQ(alert["condition"].as<std::string>(),"C");
    EXPECT_EQ(alert["data"][0]["model"]["expr"].as<std::string>(),"min(sentinel_fanout_running{job=\"sentinel-capture\"})");
    const auto evaluator=alert["data"][1]["model"]["conditions"][0]["evaluator"];
    EXPECT_EQ(evaluator["type"].as<std::string>(),"lt"); EXPECT_EQ(evaluator["params"][0].as<int>(),1);
}
