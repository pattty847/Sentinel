#include "roller/Roller.hpp"
#include "marketdata/dispatch/BookParser.hpp"
#include "legacy_v2_fixture.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <fstream>
#include <future>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Epoch=1'798'761'600'000; // 2027-01-01 UTC, aligned day
json metadata(std::string product="BTC-USD") {
    return {{"product_metadata",{{"product_id",product},{"quote_increment","0.01"},{"base_increment","0.00000001"}}}};
}
capture::Record record(int64_t ms,capture::Kind kind,std::string payload,uint64_t conn=1) {
    return {kind,{(Epoch+ms)*1'000'000,(ms+1)*1'000'000},conn,std::move(payload)};
}
std::string snapshot(std::string product="BTC-USD") {
    return json({{"channel","l2_data"},{"events",json::array({{{"type","snapshot"},{"product_id",product},
        {"updates",json::array({{{"side","bid"},{"price_level","99999.99"},{"new_quantity","2"}},
                               {{"side","offer"},{"price_level","100000.01"},{"new_quantity","3"}},
                               {{"side","bid"},{"price_level","99998"},{"new_quantity","0"}}})}}})}}).dump();
}
std::string update(int i) {
    return json({{"channel","l2_data"},{"events",json::array({{{"type","update"},{"product_id","BTC-USD"},
        {"updates",json::array({{{"side","bid"},{"price_level","99999.99"},{"new_quantity",std::to_string(2+i%7)}}})}}})}}).dump();
}
std::vector<fs::path> paths(const fs::path& root,std::string extension) {
    std::vector<fs::path> out;
    for(const auto& e:fs::recursive_directory_iterator(root)) if(e.path().extension()==extension) out.push_back(e.path());
    std::sort(out.begin(),out.end()); return out;
}
std::string contents(const fs::path& path) { std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
void save(const fs::path& path,const std::string& bytes) { std::ofstream out(path,std::ios::binary); out<<bytes; }
std::map<std::string,std::string> files(const fs::path& root) {
    std::map<std::string,std::string> out;
    for(const auto& p:paths(root,".hmc2")) out[fs::relative(p,root).string()]=contents(p);
    return out;
}
void fixture(const fs::path& root,int seconds=245,bool seal=true) {
    capture::WriterConfig c; c.root=QString::fromStdString(root.string()); c.fsyncBlocks=0;
    capture::Writer w(c,metadata());
    w.append(record(0,capture::Kind::TransportUp,"{}"));
    w.append(record(0,capture::Kind::Frame,snapshot()));
    for(int t=1;t<=seconds;++t) w.append(record(t*1000,capture::Kind::Frame,t%5==0?update(t):"{\"channel\":\"heartbeats\"}"));
    if(seal) w.close(); else w.flush();
}
RollOptions options(const fs::path& raw,const fs::path& out) { return {raw,out,"BTC-USD",Epoch,Epoch+240'000}; }
}
TEST(Roller, GridsAndClamping) {
    struct Case {const char* p;double price;const char* quote;double scale,near,deep;};
    for(const auto& c:std::vector<Case>{{"BTC-USD",100000,"0.01",100,1,5},{"ETH-USD",4000,"0.01",100,.5,2},
        {"SOL-USD",200,"0.01",100,.02,.1},{"DOGE-USD",.2,"0.00001",1e5,.00002,.0001},
        {"PEPE-USD",.00001,"0.00000001",1e8,1e-8,5e-8},{"FARTCOIN-USD",1,"0.00001",1e5,.0001,.0005},
        {"AVAX-USD",30,"0.001",1000,.002,.01}}) {
        auto m=metadata(c.p)["product_metadata"]; m["quote_increment"]=c.quote;
        m["base_increment"] = std::string(c.p)=="DOGE-USD" ? "0.1" :
            std::string(c.p)=="PEPE-USD" ? "1" : std::string(c.p)=="FARTCOIN-USD" ? "0.01" : "0.00000001";
        const auto g=deriveGrid(m,c.price);
        EXPECT_DOUBLE_EQ(g.priceScale,c.scale)<<c.p;
        EXPECT_DOUBLE_EQ(g.nearTick,c.near)<<c.p;
        EXPECT_DOUBLE_EQ(g.deepTick,c.deep)<<c.p;
        EXPECT_DOUBLE_EQ(g.sizeFloor,std::stod(m["base_increment"].get<std::string>()));
    }
    auto m=metadata("ETH-USD")["product_metadata"];
    EXPECT_DOUBLE_EQ(deriveGrid(m,100,{{"near_tick",.001}}).nearTick,.01);
    EXPECT_THROW(deriveGrid(m,0),std::runtime_error);
    EXPECT_THROW(deriveGrid(m,100,{{"price_scale",1}}),std::runtime_error);
    EXPECT_THROW(deriveGrid(m,100,{{"deep_tick",-1}}),std::runtime_error);
    // Explicit ticks still win independently of the default ladder policy.
    const auto explicitDeep=deriveGrid(m,4000,{{"deep_tick",3}});
    EXPECT_DOUBLE_EQ(explicitDeep.nearTick,.5); EXPECT_DOUBLE_EQ(explicitDeep.deepTick,3);
    const auto explicitBoth=deriveGrid(m,4000,{{"near_tick",.02},{"deep_tick",.07}});
    EXPECT_DOUBLE_EQ(explicitBoth.nearTick,.02); EXPECT_DOUBLE_EQ(explicitBoth.deepTick,.07);
    // A native increment between rungs: .03 -> .05 -> .1, then round deep to .12.
    m["quote_increment"]="0.03";
    const auto native=deriveGrid(m,100);
    EXPECT_DOUBLE_EQ(native.nearTick,.03); EXPECT_DOUBLE_EQ(native.deepTick,.12);
    m["quote_increment"]="bad"; EXPECT_THROW(deriveGrid(m,100),std::runtime_error);
}
TEST(Roller, ReaderVersionsPositionsAndOpenPolling) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString();
    fixture(root/"v1",2,false);
    JournalReader reader(root/"v1","BTC-USD"); JournalRecord r;
    std::vector<JournalRecord> records;
    while(reader.next(r)) records.push_back(r);
    ASSERT_EQ(records.size(),4); EXPECT_TRUE(reader.pending()); EXPECT_EQ(records[1].record.payload,snapshot());
    JournalReader resume(root/"v1","BTC-USD",records[1].pos);
    ASSERT_TRUE(resume.next(r)); EXPECT_EQ(r.pos,records[1].pos); EXPECT_EQ(r.record.payload,snapshot());
    auto invalid=records[1].pos; invalid.record=999;
    EXPECT_THROW({JournalReader bad(root/"v1","BTC-USD",invalid); while(bad.next(r)) {}},std::runtime_error);
    capture::WriterConfig c; c.root=QString::fromStdString((root/"v2").string()); c.fsyncBlocks=0;
    auto m=metadata(); m["connection_products"]={"BTC-USD","ETH-USD"}; m["routing"]="product-ranges-v2"; m["run_id"]="v2-fixture";m["run_started_system_ns"]=Epoch*1'000'000;
    auto w=capture::LegacyV2FixtureWriter::make(c,m);
    w->append(record(0,capture::Kind::Frame,snapshot()));
    w->append(record(1000,capture::Kind::FrameReference,"{\"sequence_gaps\":0}")); w->close();
    JournalReader v2(root/"v2","BTC-USD"); ASSERT_TRUE(v2.next(r)); EXPECT_EQ(r.version,2);
    ASSERT_TRUE(v2.next(r)); EXPECT_EQ(r.record.kind,capture::Kind::FrameReference);
    EXPECT_FALSE(v2.next(r)); EXPECT_FALSE(v2.pending());
}
TEST(Roller, PendingFramingDeferredUntilSealedOrSuperseded) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString(); fixture(root,3);
    const auto path=paths(root,".rawl2")[0]; const auto original=contents(path);
    const auto scan=capture::scan(QString::fromStdString(path.string()));
    ASSERT_GE(scan.index.size(),2);
    auto broken=original;
    broken[scan.index[0].offset]='X';
    // Sealed file: later valid framing is interior damage even in tailing mode.
    save(path,broken);
    EXPECT_THROW({capture::RecordReader reader(QString::fromStdString(path.string()),true); capture::Record r; while(reader.next(r)){}},std::runtime_error);
    // Open file with the same bytes: defer the interior-framing judgment.
    broken.resize(original.find("IDX1",scan.index.back().offset)); save(path,broken);
    capture::RecordReader open(QString::fromStdString(path.string()),true); capture::Record r;
    EXPECT_FALSE(open.next(r)); EXPECT_TRUE(open.result().pendingTail); EXPECT_FALSE(open.result().tornTail);
    { JournalReader pending(root,"BTC-USD"); JournalRecord item; EXPECT_FALSE(pending.next(item)); EXPECT_TRUE(pending.pending()); }
    // A newer run's header, independent of filenames/mtime, seals the old tail's fate.
    capture::WriterConfig cfg;cfg.root=QString::fromStdString(root.string());cfg.fsyncBlocks=0;
    auto meta=metadata();meta["run_id"]="superseding-run";
    meta["run_started_system_ns"]=scan.header.at("run_started_system_ns").get<int64_t>()+1;
    meta["connection_products"]={"BTC-USD","ETH-USD"};meta["routing"]="product-ranges-v2";
    auto newer=capture::LegacyV2FixtureWriter::make(cfg,meta);
    newer->append(record(5000,capture::Kind::Frame,snapshot()));newer->close();
    EXPECT_THROW({JournalReader superseded(root,"BTC-USD");JournalRecord item;while(superseded.next(item)){}},std::runtime_error);
    EXPECT_THROW(capture::scan(QString::fromStdString(path.string())),std::runtime_error);
    // Complete CRC errors are never pending.
    broken=original; broken[scan.index[0].offset+44]^=1; save(path,broken);
    EXPECT_THROW({capture::RecordReader bad(QString::fromStdString(path.string()),true); while(bad.next(r)){}},std::runtime_error);
    // Genuine terminal partial block remains recoverable when superseded.
    broken=original.substr(0,scan.index.back().offset+8); save(path,broken);
    EXPECT_TRUE(capture::scan(QString::fromStdString(path.string())).tornTail);
}
TEST(Roller, FeedFiltersProductsClocksAndLifecycle) {
    JournalFeed feed("BTC-USD"); int ticks=0,snaps=0,updates=0,invalid=0,trades=0;
    int64_t time=0;
    feed.onTick=[&](int64_t t){++ticks;time=t;};
    feed.onSnapshot=[&](int64_t t,int64_t local,std::vector<recording::Level> levels){++snaps;EXPECT_EQ(t,local);EXPECT_EQ(levels.size(),2);};
    feed.onUpdates=[&](int64_t,int64_t,std::vector<recording::Level>){++updates;};
    feed.onInvalid=[&](int64_t,const std::string&){++invalid;};
    feed.onTrade=[&](const Trade& t){++trades;EXPECT_EQ(t.timestamp.time_since_epoch(),std::chrono::milliseconds(Epoch+6000));};
    const auto apply=[&](int64_t ms,capture::Kind kind,std::string p){feed.apply({record(ms,kind,std::move(p)),{"BTC-USD","run",0,0},{},false,2});};
    auto withBadTime=json::parse(snapshot());withBadTime["timestamp"]="";
    apply(0,capture::Kind::Frame,withBadTime.dump());
    apply(1000,capture::Kind::Frame,snapshot("ETH-USD"));
    apply(2000,capture::Kind::BookInvalidated,"{\"product\":\"ETH-USD\"}");
    apply(3000,capture::Kind::Frame,update(0)); EXPECT_EQ(updates,1);
    apply(4000,capture::Kind::BookInvalidated,"{\"product\":\"BTC-USD\"}");
    apply(5000,capture::Kind::Frame,update(0)); EXPECT_EQ(updates,1);
    apply(6000,capture::Kind::Frame,"{\"channel\":\"market_trades\",\"events\":[{\"trades\":[{\"product_id\":\"BTC-USD\",\"price\":\"100\",\"size\":\"1\"}]}]}");
    apply(7000,capture::Kind::FrameReference,"{\"sequence_gaps\":0}");
    EXPECT_EQ(ticks,8);EXPECT_EQ(snaps,1);EXPECT_EQ(invalid,2);EXPECT_EQ(trades,1);EXPECT_EQ(time,Epoch+7000);
}
TEST(Roller, RunIdentityCrashResumeAndIdempotence) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString(); fixture(root/"raw",3665);
    auto a=options(root/"raw",root/"a"); a.toMs=Epoch+3'660'000; const auto report=roll(a);
    EXPECT_EQ(report["days"][0]["committedThroughMs"],Epoch+3'660'000);
    auto b=a; b.outputRoot=root/"b"; roll(b); EXPECT_EQ(files(a.outputRoot),files(b.outputRoot));
    auto c=a; c.outputRoot=root/"crash";
    c.afterRecordForTest=[](uint64_t n){if(n==128)throw std::runtime_error("simulated crash");};
    EXPECT_THROW(roll(c),std::runtime_error);
    ASSERT_TRUE(fs::exists(c.outputRoot/"BTC-USD"/"roller.json"));
    const auto checkpoint = json::parse(contents(c.outputRoot/"BTC-USD"/"roller.json"));
    EXPECT_LT(checkpoint.at("committedThroughMs").get<int64_t>(), Epoch+128'000);
    c.afterRecordForTest={}; roll(c); EXPECT_EQ(files(a.outputRoot),files(c.outputRoot));
    const auto before=files(c.outputRoot); roll(c); EXPECT_EQ(files(c.outputRoot),before);
    c.overrides={{"near_tick",2}}; EXPECT_THROW(roll(c),std::logic_error);
}
TEST(Roller, DryRunAndEofDoNotInventTime) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString(); fixture(root/"raw",59,false);
    auto o=options(root/"raw",root/"out"); o.dryRun=true;
    const auto report=roll(o); EXPECT_FALSE(fs::exists(o.outputRoot)); EXPECT_EQ(report["records"],60);
    o.dryRun=false; roll(o); EXPECT_TRUE(files(o.outputRoot).empty());
    // A later snapshot must not change the dry-run day's initially derived grid.
    capture::WriterConfig cfg; cfg.root=QString::fromStdString((root/"eth").string());cfg.symbol="ETH-USD";cfg.fsyncBlocks=0;
    capture::Writer writer(cfg,metadata("ETH-USD"));
    auto snap=json::parse(snapshot("ETH-USD"));
    snap["events"][0]["updates"][0]["price_level"]="100";
    snap["events"][0]["updates"][1]["price_level"]="102";
    writer.append(record(0,capture::Kind::Frame,snap.dump()));
    snap["events"][0]["updates"][0]["price_level"]="200";
    snap["events"][0]["updates"][1]["price_level"]="202";
    writer.append(record(30'000,capture::Kind::Frame,snap.dump()));writer.close();
    o.journalRoot=root/"eth";o.product="ETH-USD";o.dryRun=true;
    EXPECT_EQ(roll(o)["days"][0]["grid"]["nearTick"],.01);
}
TEST(Roller, BlockingQueueAdmitsAtomicOversizedSnapshot) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); auto c=deriveGrid(metadata()["product_metadata"],100000).config(temp.path().toStdString());
    c.maxQueuedLevels=1; c.latenessMs=0;
    recording::BookRecorder r(c);
    r.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999.99,2},{false,100000.01,3}});
    for(int t=1;t<=10000;++t) r.onUpdatesAt("BTC-USD",Epoch+t,Epoch+t,{{true,99999.99,double(2+t%3)}});
    r.onTick(Epoch+60'000); r.drain(); EXPECT_EQ(r.stats().queueDrops,0);EXPECT_EQ(r.stats().invalidations,0);
    EXPECT_EQ(r.stats().columnsWritten,2);
}
TEST(Roller, CommitFloorAndCeiling) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); auto c=deriveGrid(metadata()["product_metadata"],100000).config(temp.path().toStdString());
    c.commitFloorMs=Epoch+60'000;c.commitCeilingMs=Epoch+120'000;c.latenessMs=0;
    recording::BookRecorder r(c);r.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999.99,2},{false,100000.01,3}});
    r.onTick(Epoch+180'000);r.drain();
    auto rows=recording::Hmc2Store::readRange(c.root,"BTC-USD","near",60'000,Epoch,Epoch+180'000);
    ASSERT_EQ(rows.size(),1);EXPECT_EQ(rows[0].bucketStartMs,Epoch+60'000);
}
TEST(Roller, DiffQualifiesMasksLateAndDetectsContent) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString();fixture(root/"raw");
    auto a=options(root/"raw",root/"a");roll(a);
    fs::copy(a.outputRoot,root/"b",fs::copy_options::recursive);
    auto rows=recording::Hmc2Store::readRange(root/"b","BTC-USD","near",60'000,Epoch,Epoch+240'000);
    ASSERT_EQ(rows.size(),4);
    { recording::Hmc2Store s(root/"b");rows[1].flags|=recording::kLateEvents;s.append(rows[1]); }
    auto report=diff(root/"a",root/"b","BTC-USD","near",Epoch,Epoch+240'000);
    EXPECT_EQ(report["qualifying"],3);EXPECT_EQ(report["matching"],3);EXPECT_EQ(report["nonqualifying"],1);
    { recording::Hmc2Store s(root/"b");++rows[2].entries[0].twapCode;s.append(rows[2]); }
    report=diff(root/"a",root/"b","BTC-USD","near",Epoch,Epoch+240'000);EXPECT_EQ(report["mismatching"],1);
    const auto& d=report["minutes"][2]["difference"];
    EXPECT_EQ(d["twapCodeDeltaHistogram"]["1"],1);
    EXPECT_EQ(d["twapCodeDeltaHistogram"]["0"],rows[2].entries.size()-1);
    for(bool ask:{false,true}) {
        long double before=0,after=0;
        for(size_t i=0;i<rows[2].entries.size();++i) {
            const auto& e=rows[2].entries[i];if(e.isAsk!=ask)continue;
            before+=recording::decodeSize(e.twapCode-(i==0),rows[2].header.sizeScale);
            after+=recording::decodeSize(e.twapCode,rows[2].header.sizeScale);
        }
        const auto& total=d["totalTwap"][ask?"ask":"bid"];
        EXPECT_DOUBLE_EQ(total["a"].get<double>(),double(before));
        EXPECT_DOUBLE_EQ(total["b"].get<double>(),double(after));
        EXPECT_DOUBLE_EQ(total["delta"].get<double>(),double(after-before));
        EXPECT_DOUBLE_EQ(total["relativeDelta"].get<double>(),double((after-before)/before));
    }
}

TEST(Roller, SilenceAcrossMidnightCommitsMinuteAndHour) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/boundary-XXXXXX"));
    ASSERT_TRUE(temp.isValid()); const fs::path root=temp.path().toStdString();
    constexpr int64_t day=86'400'000;
    capture::WriterConfig cfg; cfg.root=QString::fromStdString((root/"raw").string()); cfg.fsyncBlocks=0;
    capture::Writer writer(cfg,metadata());
    writer.append(record(day-120'000,capture::Kind::Frame,snapshot()));
    writer.append(record(day-30'000,capture::Kind::BookInvalidated,"{\"product\":\"BTC-USD\"}"));
    writer.append(record(day+120'000,capture::Kind::Frame,snapshot()));
    writer.append(record(day+182'000,capture::Kind::Frame,"{\"channel\":\"heartbeats\"}"));
    writer.close();
    RollOptions o{root/"raw",root/"two-days","BTC-USD",Epoch+day-120'000,Epoch+day+180'000};
    roll(o);
    const auto check = [&](const fs::path& output) {
        for (const auto* layer:{"near","deep"}) {
            const auto minutes=recording::Hmc2Store::readRange(output,"BTC-USD",layer,60'000,Epoch+day-120'000,Epoch+day);
            ASSERT_EQ(minutes.size(),2); EXPECT_EQ(minutes.back().bucketStartMs,Epoch+day-60'000);
            EXPECT_EQ(minutes.back().observedMs,30'000);
        }
        const auto hours=recording::Hmc2Store::readRange(output,"BTC-USD","deep",3'600'000,Epoch+day-3'600'000,Epoch+day);
        ASSERT_EQ(hours.size(),1); EXPECT_EQ(hours[0].observedMs,90'000);
    };
    check(o.outputRoot);
    o.outputRoot=root/"to-midnight"; o.toMs=Epoch+day; roll(o); check(o.outputRoot);
    // A non-day --to boundary must retain the same partial minute too.
    o.outputRoot=root/"to-minute";o.toMs=Epoch+day-60'000; roll(o);
    const auto rows=recording::Hmc2Store::readRange(o.outputRoot,"BTC-USD","near",60'000,o.fromMs,o.toMs);
    ASSERT_EQ(rows.size(),1); EXPECT_EQ(rows[0].observedMs,60'000);
}

TEST(Roller, SequenceErrorAllowsFollowingSnapshotWithoutTransportUp) {
    for (const uint64_t bad : {uint64_t{13},uint64_t{9},uint64_t{10}}) {
        JournalFeed feed("BTC-USD");int snapshots=0,updates=0;
        feed.onSnapshot=[&](int64_t,int64_t,std::vector<recording::Level>){++snapshots;};
        feed.onUpdates=[&](int64_t,int64_t,std::vector<recording::Level>){++updates;};
        const auto apply=[&](uint64_t seq,const std::string& payload) {
            auto j=json::parse(payload); j["sequence_num"]=seq;
            feed.apply({record(0,capture::Kind::Frame,j.dump()),{"BTC-USD","run",0,0},{},false,1});
        };
        apply(10,snapshot()); apply(bad,update(1));
        apply(bad+1,snapshot()); apply(bad+2,update(2));
        EXPECT_EQ(snapshots,2); EXPECT_EQ(updates,1);
    }
}

TEST(Roller, StopWakesBlockedProducerBeforeWorkerDrains) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/shutdown-XXXXXX")); ASSERT_TRUE(temp.isValid());
    auto cfg=deriveGrid(metadata()["product_metadata"],100000).config(temp.path().toStdString());
    cfg.maxQueuedLevels=1;
    std::promise<void> workerEntered,releaseWorker,producerWaiting;
    auto release=releaseWorker.get_future().share();std::atomic<bool> first{true};int waiting=0;
    cfg.publisher=[](auto){};
    cfg.beforePublicationForTest=[&](bool){if(first.exchange(false)){workerEntered.set_value();release.wait();}};
    cfg.beforeQueueWaitForTest=[&]{if(++waiting==2)producerWaiting.set_value();};
    recording::BookRecorder recorder(cfg);
    recorder.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999.99,2},{false,100000.01,3}});recorder.drain();
    recorder.onTick(Epoch+1000);
    workerEntered.get_future().wait();
    recorder.onUpdatesAt("BTC-USD",Epoch+1001,Epoch+1001,{{true,99999.99,4}});
    const auto enqueue=[&]{
        try {recorder.onUpdatesAt("BTC-USD",Epoch+1002,Epoch+1002,{{true,99999.99,5}});return false;}
        catch(const std::runtime_error& e){return std::string(e.what())=="recorder stopped";}
    };
    auto producer=std::async(std::launch::async,enqueue);
    auto secondProducer=std::async(std::launch::async,enqueue);
    producerWaiting.get_future().wait();
    recorder.requestStop();
    const auto status=producer.wait_for(std::chrono::seconds(2));
    const auto secondStatus=secondProducer.wait_for(std::chrono::seconds(2));
    // Release on failure too: the mutation test must fail rather than deadlock.
    releaseWorker.set_value();
    EXPECT_EQ(status,std::future_status::ready);EXPECT_EQ(secondStatus,std::future_status::ready);
    EXPECT_TRUE(producer.get());EXPECT_TRUE(secondProducer.get());
}

TEST(Roller, RefusesUnownedHmc2AndConfiguredRecorderRoots) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/roots-XXXXXX"));ASSERT_TRUE(temp.isValid());
    const fs::path root=temp.path().toStdString(); fixture(root/"raw");
    fs::create_directories(root/"unowned"/"BTC-USD"/"near");
    const auto marker=root/"unowned"/"BTC-USD"/"near"/"existing.hmc2";save(marker,"do not touch");
    auto o=options(root/"raw",root/"unowned");
    EXPECT_THROW(roll(o),std::runtime_error);EXPECT_EQ(contents(marker),"do not touch");
    // Exercise the real CLI config discovery from a temporary working directory.
    const auto previous=fs::current_path();
    struct Restore {fs::path path;~Restore(){fs::current_path(path);}} restore{previous};
    fs::create_directories(root/"config");
    save(root/"config"/"server_config.yaml","recording:\n  dir: '"+(root/"live").string()+"'\n");
    fs::current_path(root);
    auto invoke=[&](const fs::path& output) {
        std::vector<std::string> args={"sentinel-roll",(root/"raw").string(),output.string(),"--products","BTC-USD",
            "--from","2027-01-01","--to","2027-01-02"};
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());
        return rollMain(int(argv.size()),argv.data());
    };
    EXPECT_EQ(invoke(root/"live"),1); EXPECT_FALSE(fs::exists(root/"live"));
    fs::create_directory(root/"live");
    fs::create_directory_symlink(root/"live",root/"live-alias");
    EXPECT_EQ(invoke(root/"live-alias"),1);
    EXPECT_EQ(invoke(root/"unowned"),1);EXPECT_EQ(contents(marker),"do not touch");
}

// A6: the history publisher and its publication mode are not checkpoint
// policy. The slice A..C BTC-USD product root keeps its hash, so existing roots
// reopen under the serving path (a changed hash throws "checkpoint policy/range mismatch").
TEST(Roller, PublisherKeepsSliceACheckpointPolicyHash) {
    constexpr uint64_t SliceABtcHash=18293455670244139256ull; // /Volumes/T7/sentinel-data/hmc2/BTC-USD/roller.json
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX")); fs::path root=temp.path().toStdString(); fixture(root/"raw");
    auto o=options(root/"raw",root/"out"); o.toMs=Epoch+120'000; o.productWriterLease=true;
    roll(o);
    const auto cp=root/"out"/"BTC-USD"/"roller.json";
    EXPECT_EQ(json::parse(contents(cp)).at("configHash").get<uint64_t>(),SliceABtcHash);
    std::mutex mutex; std::vector<std::shared_ptr<const recording::Hmc2Record>> finals;
    o.toMs=Epoch+240'000;
    o.publisher=[&](std::shared_ptr<const recording::Hmc2Record> r){std::lock_guard lock(mutex);finals.push_back(std::move(r));};
    EXPECT_NO_THROW(roll(o));
    EXPECT_EQ(json::parse(contents(cp)).at("configHash").get<uint64_t>(),SliceABtcHash);
    EXPECT_EQ(json::parse(contents(cp)).at("committedThroughMs").get<int64_t>(),Epoch+240'000);
    // History publishes committed minutes only (2 and 3, both layers), once each.
    ASSERT_EQ(finals.size(),4u);
    for(const auto& r:finals){EXPECT_FALSE(r->flags&recording::kProvisional);EXPECT_EQ(r->committedThroughMs,r->bucketStartMs+60'000);}
}
// The lead fork shares no store: it coexists with the history recorder's
// product lease, never writes, and claims no committed cutoff.
TEST(Roller, LeadForkNeverPersistsAndClaimsNoCommit) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/test-XXXXXX"));
    auto c=deriveGrid(metadata()["product_metadata"],100000).config(temp.path().toStdString());
    c.writerProduct="BTC-USD"; c.latenessMs=0;
    std::vector<std::shared_ptr<const recording::Hmc2Record>> finals,lead;
    c.publisher=[&](auto r){finals.push_back(std::move(r));};
    c.publication=recording::RecorderConfig::Publication::Finals;
    recording::BookRecorder history(c);
    history.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999.99,2},{false,100000.01,3}});
    history.onTick(Epoch+30'000);
    auto fork=history.forkLead([&](auto r){lead.push_back(std::move(r));},500);
    fork->onUpdatesAt("BTC-USD",Epoch+31'000,Epoch+31'000,{{false,100003,7}});
    fork->onTick(Epoch+90'000); fork->drain();
    history.onTick(Epoch+90'000); history.drain();
    ASSERT_FALSE(lead.empty());
    bool held=false;
    for(const auto& r:lead){
        EXPECT_TRUE(r->flags&recording::kProvisional); EXPECT_EQ(r->committedThroughMs,0);
        held=held||(r->header.layer=="near"&&r->bucketStartMs==Epoch&&r->observedMs==60'000&&
            std::any_of(r->entries.begin(),r->entries.end(),[](const auto& e){return e.isAsk&&e.row==100003;}));
    }
    EXPECT_TRUE(held);
    ASSERT_EQ(finals.size(),2u);
    const auto rows=recording::Hmc2Store::readRange(c.root,"BTC-USD","near",60'000,Epoch,Epoch+120'000);
    ASSERT_EQ(rows.size(),1u); // history alone persisted minute 0, without the lead-only level
    EXPECT_TRUE(std::none_of(rows[0].entries.begin(),rows[0].entries.end(),[](const auto& e){return e.row==100003;}));
    EXPECT_EQ(fork->stats().columnsWritten,0u);
}
// After the flip the roller root is live: unscoped batch is refused there, and
// --product-lease (shared root, exclusive product) may repair beside it.
TEST(Roller, RollCliProductLeaseMayWriteRollerServedRoot) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/roots-XXXXXX"));ASSERT_TRUE(temp.isValid());
    const fs::path root=temp.path().toStdString(); fixture(root/"raw");
    const auto previous=fs::current_path();
    struct Restore {fs::path path;~Restore(){fs::current_path(path);}} restore{previous};
    fs::create_directories(root/"config");
    save(root/"config"/"server_config.yaml","recording:\n  dir: '"+(root/"primary").string()+"'\n  source: roller\n"
         "roller_shadow:\n  dir: '"+(root/"hmc2").string()+"'\n");
    fs::current_path(root);
    auto invoke=[&](bool lease) {
        std::vector<std::string> args={"sentinel-roll",(root/"raw").string(),(root/"hmc2").string(),"--products","BTC-USD",
            "--from","2027-01-01T00:00:00Z","--to","2027-01-01T00:04:00Z"};
        if(lease) args.push_back("--product-lease");
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());
        return rollMain(int(argv.size()),argv.data());
    };
    EXPECT_EQ(invoke(false),1); EXPECT_FALSE(fs::exists(root/"hmc2"));
    EXPECT_EQ(invoke(true),0);
    EXPECT_TRUE(fs::exists(root/"hmc2"/".writer-BTC-USD.lock"));
    EXPECT_FALSE(recording::Hmc2Store::readRange(root/"hmc2","BTC-USD","near",60'000,Epoch,Epoch+240'000).empty());
}

TEST(Roller, RealJournalCrashResumeRestoresLargeDeltaBase) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/real-resume-XXXXXX"));ASSERT_TRUE(temp.isValid());
    const fs::path root=temp.path().toStdString();fs::create_directories(root/"raw");
    fs::copy_file(ROLLER_FIXTURE,root/"raw"/"btc.rawl2");
    const auto from=parseTime("2026-10-01T04:53:00Z"),to=from+180'000;
    RollOptions o{root/"raw",root/"clean","BTC-USD",from,to};roll(o);
    JournalReader reader(root/"raw","BTC-USD");JournalRecord item;uint64_t crashAt=0;
    while(reader.next(item)){++crashAt;if(item.record.time.systemNs/1'000'000>=from+130'000)break;}
    auto resumed=o;resumed.outputRoot=root/"resumed";
    resumed.afterRecordForTest=[&](uint64_t n){if(n==crashAt)throw std::runtime_error("simulated crash");};
    EXPECT_THROW(roll(resumed),std::runtime_error);
    const auto cp=json::parse(contents(resumed.outputRoot/"BTC-USD"/"roller.json"));
    const auto persisted=recording::Hmc2Store::readRange(resumed.outputRoot,"BTC-USD","deep",60'000,from,to);
    ASSERT_GE(persisted.size(),2);EXPECT_GT(persisted.back().entries.size(),1000);
    EXPECT_GT(persisted.back().bucketStartMs+60'000,cp["committedThroughMs"].get<int64_t>());
    resumed.afterRecordForTest={};roll(resumed);
    EXPECT_EQ(files(o.outputRoot),files(resumed.outputRoot));
    const auto before=files(resumed.outputRoot);roll(resumed);EXPECT_EQ(files(resumed.outputRoot),before);
}

TEST(Roller, CapturedHourBoundaryWaitsForCommittedWatermark) {
    QTemporaryDir temp(QStringLiteral(ROLLER_TEST_ROOT "/real-boundary-XXXXXX"));ASSERT_TRUE(temp.isValid());
    const fs::path root=temp.path().toStdString(),fixtures=fs::path(ROLLER_FIXTURE).parent_path();
    fs::create_directories(root/"raw");
    // Original captured records around the reported 16:00 boundary, reblocked
    // for compression only. No timestamp shifts, omitted records or synthetic ticks.
    fs::copy_file(fixtures/"btc-hour-boundary.rawl2",root/"raw"/"btc.rawl2");
    const auto from=parseTime("2026-10-01T15:00:00Z"),end=from+3'600'000;
    RollOptions o{root/"raw",root/"out","BTC-USD",from,end};
    const auto report=roll(o);
    EXPECT_EQ(report["days"][0]["committedThroughMs"],end);
    for(const auto* layer:{"near","deep"}) {
        const auto minutes=recording::Hmc2Store::readRange(o.outputRoot,"BTC-USD",layer,60'000,from,end);
        ASSERT_EQ(minutes.size(),36); // Snapshot starts at 15:24; earlier minutes are unknown.
        EXPECT_EQ(minutes.back().bucketStartMs,end-60'000);EXPECT_EQ(minutes.back().observedMs,60'000);
    }
    const auto hours=recording::Hmc2Store::readRange(o.outputRoot,"BTC-USD","deep",3'600'000,from,end);
    ASSERT_EQ(hours.size(),1);EXPECT_EQ(hours[0].bucketStartMs,from);
    EXPECT_GT(hours[0].observedMs,35*60'000);EXPECT_LT(hours[0].observedMs,36*60'000);
    const auto before=files(o.outputRoot);
    const auto checkpoint=contents(o.outputRoot/"BTC-USD"/"roller.json");
    EXPECT_EQ(roll(o)["columnsWritten"],0);
    EXPECT_EQ(files(o.outputRoot),before);EXPECT_EQ(contents(o.outputRoot/"BTC-USD"/"roller.json"),checkpoint);
}
