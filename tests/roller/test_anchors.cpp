#include "roller/AnchorStore.hpp"
#include "roller/Roller.hpp"
#include "roller/Grid.hpp"
#include "roller/JournalFeed.hpp"
#include "servermodel/HmcolFormat.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QDateTime>
#include <QTimeZone>
#include <atomic>
#include <fstream>
#include <iostream>
#include <numeric>

using namespace sentinel;
using namespace sentinel::roller;
using namespace recording;
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
constexpr int64_t Day = 86'400'000, Minute = 60'000, Quarter = 900'000;
constexpr int64_t Epoch = 1'791'158'400'000; // 2026-10-05 UTC
json meta(const std::string& p = "BTC-USD") {
    return {{"product_id",p},{"quote_increment","0.01"},{"base_increment","0.00000001"}};
}
std::map<std::string,std::string> files(const fs::path& root) {
    std::map<std::string,std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(root)) if (e.path().extension() == ".hmc2") {
        std::ifstream in(e.path(),std::ios::binary);
        out[fs::relative(e.path(),root).string()] = {std::istreambuf_iterator<char>(in),{}};
    }
    return out;
}
void copyPrefix(const fs::path& a, const fs::path& b) {
    fs::create_directories(b);
    for (const auto& e : fs::recursive_directory_iterator(a)) if (e.path().extension() == ".hmc2") {
        const auto dst = b/fs::relative(e.path(),a);
        fs::create_directories(dst.parent_path()); fs::copy_file(e.path(),dst);
    }
}
ReplayAnchor anchor() {
    ReplayAnchor a;
    a.identity = {"BTC-USD",Epoch,Epoch,Epoch+Day,123}; a.boundaryMs = Epoch;
    a.pos = {"BTC-USD","run",15,3}; a.gridSnapshot = {"BTC-USD","previous-run",0,1};
    a.referenceMid = 100000; a.metadata = meta();
    a.feed.bytes = {1,2,3}; a.book.bytes = {4,5}; a.recorder.bytes = {6,7,8};
    return a;
}
void sameWatermarks(BookRecorder& a, BookRecorder& b, const std::string& p) {
    for (const auto* layer : {"near","deep"}) {
        const auto x=a.watermarks(p,layer), y=b.watermarks(p,layer);
        EXPECT_EQ(x.minuteThroughMs,y.minuteThroughMs); EXPECT_EQ(x.hourThroughMs,y.hourThroughMs);
        EXPECT_EQ(x.lastColumnMs,y.lastColumnMs);
    }
}
void fixStateCrc(std::vector<uint8_t>& b) {
    const auto crc=hmcol::crc32(b.data(),b.size()-4);
    for (unsigned i=0;i<4;++i) b[b.size()-4+i]=uint8_t(crc>>(i*8));
}
}
TEST(AnchorStore, RoundTripAndAtomicReplacement) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); auto a=anchor();
    AnchorStore::write(root,a);
    const auto path=AnchorStore::path(root,a);
    EXPECT_EQ(path.filename(),"0000.anchor");
    std::string reason;
    auto loaded=AnchorStore::read(path,a.identity,a.boundaryMs,a.pos,&reason);
    ASSERT_TRUE(loaded) << reason;
    EXPECT_EQ(AnchorStore::encode(*loaded),AnchorStore::encode(a));
    a.recorder.bytes={9,8,7}; AnchorStore::write(root,a);
    loaded=AnchorStore::read(path,a.identity,a.boundaryMs,a.pos);
    ASSERT_TRUE(loaded); EXPECT_EQ(loaded->recorder.bytes,a.recorder.bytes);
    EXPECT_EQ(std::distance(fs::directory_iterator(path.parent_path()),fs::directory_iterator{}),1);
}
TEST(AnchorStore, RejectsCorruptionVersionsIdentityAndFuturePositions) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); auto a=anchor();
    AnchorStore::write(root,a); const auto path=AnchorStore::path(root,a);
    auto b=AnchorStore::encode(a); b.resize(b.size()-1); EXPECT_THROW(AnchorStore::decode(b),std::runtime_error);
    b=AnchorStore::encode(a); b[b.size()-1]^=1; EXPECT_THROW(AnchorStore::decode(b),std::runtime_error);
    b=AnchorStore::encode(a); b[8]=2; EXPECT_THROW(AnchorStore::decode(b),std::runtime_error);
    b=AnchorStore::encode(a); b[0]^=1; EXPECT_THROW(AnchorStore::decode(b),std::runtime_error);
    b=AnchorStore::encode(a); b.push_back(0); EXPECT_THROW(AnchorStore::decode(b),std::runtime_error);
    auto expected=a.identity; ++expected.configHash; EXPECT_FALSE(AnchorStore::read(path,expected,a.boundaryMs));
    expected=a.identity; expected.fromMs+=Minute; EXPECT_FALSE(AnchorStore::read(path,expected,a.boundaryMs));
    expected=a.identity; expected.product="PEPE-USD"; EXPECT_FALSE(AnchorStore::read(path,expected,a.boundaryMs));
    auto ceiling=a.pos; --ceiling.record; EXPECT_FALSE(AnchorStore::read(path,a.identity,a.boundaryMs,ceiling));
    ceiling=a.pos; ceiling.run="other"; EXPECT_FALSE(AnchorStore::read(path,a.identity,a.boundaryMs,ceiling));
    EXPECT_FALSE(AnchorStore::read(path,a.identity,a.boundaryMs-1));
    EXPECT_FALSE(AnchorStore::read(path.string()+"missing",a.identity,a.boundaryMs));
    a.feed.version=2; EXPECT_THROW(AnchorStore::encode(a),std::runtime_error);
}
TEST(RecorderState, SyntheticContinuationPreservesEveryFieldAndOutput) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    auto cfg=deriveGrid(meta(),100000).config(root/"a"); cfg.commitFloorMs=Epoch;
    BookRecorder original(cfg);
    original.onSnapshotAt("BTC-USD",Epoch,Epoch+137,{{true,99999,0.123456789012345},{false,100001,3}});
    auto send=[&](BookRecorder& r,int64_t t) {
        const auto local=Epoch+t+137;
        if (t==910000) r.onInvalid("BTC-USD",local,"test invalidation");
        else if (t==920000) r.onSnapshotAt("BTC-USD",Epoch+t,local,{{true,99999,1.1},{false,100001,3.1}});
        else {
            r.onUpdatesAt("BTC-USD",Epoch+t-(t%7000==0?2200:0),local,
                {{true,99999,double(t%17)*0.100000000000001},{false,100001,t==930000?0:3.1}});
        }
        r.onTick(local);
    };
    for (int64_t t=1000;t<=Quarter;t+=1000) send(original,t);
    auto bytes=original.exportState(); copyPrefix(root/"a",root/"b");
    cfg.root=root/"b"; BookRecorder restored(cfg); restored.importState(bytes);
    EXPECT_EQ(bytes,restored.exportState()); sameWatermarks(original,restored,"BTC-USD");
    for (int64_t t=Quarter+1000;t<=7'203'000;t+=1000) {
        send(original,t); send(restored,t);
        if (t%Quarter==0 || t==910000 || t==930000) {
            const auto state=original.exportState(); restored.drain();
            EXPECT_EQ(state,restored.exportState()) << t;
        }
    }
    original.drain(); restored.drain(); sameWatermarks(original,restored,"BTC-USD");
    EXPECT_TRUE(files(root/"a")==files(root/"b"));
    const auto hours=Hmc2Store::readRange(root/"b","BTC-USD","deep",3'600'000,Epoch,Epoch+7'200'000);
    EXPECT_EQ(hours.size(),2);
}
TEST(RecorderState, InvalidOneSidedAndPublicationBoundaries) {
    for (int mode=0;mode<3;++mode) {
        QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
        auto cfg=deriveGrid(meta(),100000).config(root/"a");
        cfg.commitFloorMs=Epoch;
        std::atomic<size_t> publishes{0}, invalidations{0};
        cfg.publisher=[&](auto) { ++publishes; };
        cfg.onSelfInvalidated=[&](const auto&,const auto&) { ++invalidations; };
        BookRecorder a(cfg);
        a.onSnapshotAt("BTC-USD",Epoch,Epoch+177,{{true,99999,0.125},{false,100001,3}});
        a.onUpdatesAt("BTC-USD",Epoch+60001,Epoch+60178,{{false,100001,0}});
        if(mode==1) a.onInvalid("BTC-USD",Epoch+60179,"upstream gap");
        if(mode==2) a.onTick(Epoch+67000);
        const auto state=a.exportState(); copyPrefix(root/"a",root/"b");
        cfg.root=root/"b"; BookRecorder b(cfg);
        const auto before=std::pair(publishes.load(),invalidations.load());
        b.importState(state);
        EXPECT_EQ(before,std::pair(publishes.load(),invalidations.load()));
        EXPECT_EQ(state,b.exportState()) << "mode=" << mode;
        for (auto* r:{&a,&b}) {
            r->onTick(Epoch+91000);
            r->onSnapshotAt("BTC-USD",Epoch+92000,Epoch+92177,{{true,99999,2},{false,100001,3}});
            r->onTick(Epoch+183000);
        }
        a.drain(); b.drain(); sameWatermarks(a,b,"BTC-USD");
        EXPECT_EQ(a.exportState(),b.exportState()); EXPECT_TRUE(files(root/"a")==files(root/"b"));
    }
}
TEST(RecorderState, AccumulatorsRoundTripWithoutRounding) {
    QTemporaryDir tmp;
    auto cfg=deriveGrid(meta(),100000).config(tmp.path().toStdString());
    BookRecorder src(cfg);
    constexpr double quantity=0.123456789012345;
    src.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999,quantity},{false,100001,3}});
    auto bytes=src.exportState();
    const auto j=json::from_cbor(bytes.begin(),bytes.end()-4);
    const auto& rows=j.at("symbols").at("BTC-USD").at("layers").at(0).at("rows");
    bool checked=false;
    for (const auto& row:rows) if (!row.at(1).get<bool>()) {
        EXPECT_EQ(std::stold(row.at(2).get<std::string>()),static_cast<long double>(quantity));
        checked=true;
    }
    EXPECT_TRUE(checked);
}
TEST(RecorderState, RejectsBadStateWithoutInstallingOrPublishing) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    auto cfg=deriveGrid(meta(),100000).config(root/"a"); BookRecorder src(cfg);
    src.onSnapshotAt("BTC-USD",Epoch,Epoch,{{true,99999,2},{false,100001,3}});
    const auto state=src.exportState(); cfg.root=root/"b";
    int publications=0; cfg.publisher=[&](auto){++publications;};
    BookRecorder dst(cfg); const auto empty=dst.exportState();
    auto bad=state; bad[bad.size()-1]^=1; EXPECT_THROW(dst.importState(bad),std::runtime_error);
    EXPECT_EQ(dst.exportState(),empty);
    auto j=json::from_cbor(state.begin(),state.end()-4); j["version"]=2;
    bad=json::to_cbor(j); bad.resize(bad.size()+4); fixStateCrc(bad);
    EXPECT_THROW(dst.importState(bad),std::runtime_error); EXPECT_EQ(dst.exportState(),empty);
    j["version"]=1; j["symbols"]["BTC-USD"]["layers"][1]["header"]["configHash"]=0;
    bad=json::to_cbor(j); bad.resize(bad.size()+4); fixStateCrc(bad);
    EXPECT_THROW(dst.importState(bad),std::runtime_error); EXPECT_EQ(dst.exportState(),empty);
    dst.importState(state); EXPECT_EQ(publications,0); EXPECT_EQ(dst.exportState(),state);
    EXPECT_THROW(dst.importState(state),std::runtime_error);
    cfg.root=root/"c"; ++cfg.latenessMs; BookRecorder wrong(cfg);
    EXPECT_THROW(wrong.importState(state),std::runtime_error);
    --cfg.latenessMs; cfg.root=root/"d"; cfg.writerProduct="PEPE-USD";
    BookRecorder wrongScope(cfg);
    EXPECT_THROW(wrongScope.importState(state),std::runtime_error);
}
TEST(JournalSeek, IndexedAndUnsealedDecodeOnlySelectedBlockAndSuffix) {
    for (bool sealed : {true,false}) {
        QTemporaryDir tmp; fs::path root=tmp.path().toStdString();
        capture::WriterConfig cfg; cfg.root=QString::fromStdString(root.string()); cfg.fsyncBlocks=0;
        capture::Writer writer(cfg,{{"product_metadata",meta()}});
        for (int i=0;i<100;++i) {
            writer.append({capture::Kind::Frame,{(Epoch+i*1000)*1'000'000,i*1'000'000LL},1,"{}"});
            if (i%10==9) writer.flush();
        }
        if (sealed) writer.close();
        JournalReader all(root,"BTC-USD"); JournalRecord r; std::vector<JournalRecord> records;
        while(all.next(r)) records.push_back(r);
        ASSERT_EQ(records.size(),100);
        JournalReader seek(root,"BTC-USD",records[75].pos); size_t n=75;
        while(seek.next(r)) { ASSERT_LT(n,records.size()); EXPECT_EQ(r.pos,records[n].pos); EXPECT_EQ(r.record,records[n++].record); }
        EXPECT_EQ(n,100); EXPECT_EQ(seek.decodedRecords(),30); EXPECT_EQ(all.decodedRecords(),100);
        auto bad=records.back().pos; bad.record=100;
        EXPECT_THROW({JournalReader missing(root,"BTC-USD",bad); missing.next(r);},std::runtime_error);
    }
}
TEST(JournalSeek, CorruptSelectedBlockAndIndexAreRejected) {
    QTemporaryDir tmp; fs::path root=tmp.path().toStdString();
    capture::WriterConfig cfg; cfg.root=QString::fromStdString(root.string()); cfg.fsyncBlocks=0;
    capture::Writer writer(cfg,{{"product_metadata",meta()}});
    for (int i=0;i<4;++i) {
        writer.append({capture::Kind::Frame,{(Epoch+i*1000)*1'000'000,i*1'000'000LL},1,"{}"}); writer.flush();
    }
    writer.close(); JournalReader reader(root,"BTC-USD");
    const auto path=reader.files().front().path;
    const auto scan=capture::scan(QString::fromStdString(path.string()));
    ASSERT_EQ(scan.index.size(),4);
    std::ifstream in(path,std::ios::binary); const std::string original{std::istreambuf_iterator<char>(in),{}};
    const auto save=[&](const std::string& bytes) { std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(bytes.data(),bytes.size()); };
    auto bytes=original; bytes[scan.index[2].offset+48]^=1; save(bytes);
    capture::Record r;
    EXPECT_THROW({capture::RecordReader bad(QString::fromStdString(path.string()),false,scan.index[2].ordinal); bad.next(r);},std::runtime_error);
    bytes=original; bytes.back()^=1; save(bytes);
    EXPECT_THROW({capture::RecordReader bad(QString::fromStdString(path.string()),false,scan.index[2].ordinal); while (bad.next(r)) {}},std::runtime_error);
    size_t damagedIndexes=0;
    capture::RecordReader recovering(QString::fromStdString(path.string()),false,scan.index[2].ordinal,
        [&](const auto&,const char*) { ++damagedIndexes; });
    while (recovering.next(r)) {}
    EXPECT_EQ(damagedIndexes,1);
    EXPECT_EQ(recovering.result().index.size(),4);
    EXPECT_EQ(recovering.result().seekSkippedBlocks,2);
    // Earlier payloads belong to the trusted prefix for a seek; a full scan
    // still rejects their CRC failure. No relaxation of the default verifier.
    bytes=original; bytes[scan.index[0].offset+48]^=1; save(bytes);
    EXPECT_THROW(capture::scan(QString::fromStdString(path.string())),std::runtime_error);
    capture::RecordReader seek(QString::fromStdString(path.string()),false,scan.index[2].ordinal);
    EXPECT_TRUE(seek.next(r)); EXPECT_EQ(r.time.systemNs,(Epoch+2000)*1'000'000);
}
TEST(RecorderState, RealBtcAndPepeHour) {
    const char* input=std::getenv("SENTINEL_ANCHOR_REAL_ROOT");
    if (!input) GTEST_SKIP() << "set SENTINEL_ANCHOR_REAL_ROOT for read-only captured-hour validation";
    for (const auto* product : {"BTC-USD","PEPE-USD"}) {
        QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
        const fs::path raw=fs::path(input)/product/"2026"/"10"/"05";
        JournalReader reader(raw,product); JournalFeed feed(product); JournalRecord r;
        std::unique_ptr<BookRecorder> a,b; RecorderConfig cfg; ReplayAnchor saved;
        int64_t first=0, boundary=0, end=0; size_t suffix=0;
        feed.onSnapshot=[&](int64_t t,int64_t local,std::vector<Level> levels) {
            if (!a) {
                double bid=0,ask=std::numeric_limits<double>::infinity();
                for (const auto& l:levels) if(l.size>0) { if(l.isBid) bid=std::max(bid,l.price); else ask=std::min(ask,l.price); }
                if (!(bid>0) || !std::isfinite(ask)) return;
                const auto mid=std::midpoint(bid,ask);
                cfg=deriveGrid(r.metadata,mid).config(root/"a");
                first=local; boundary=(first/Quarter+1)*Quarter; end=(first/3'600'000+2)*3'600'000+5000;
                cfg.commitFloorMs=first/Minute*Minute;
                a=std::make_unique<BookRecorder>(cfg);
                saved.identity={product,first/Day*Day,first/Day*Day,first/Day*Day+Day,123};
                saved.referenceMid=mid; saved.gridSnapshot=r.pos; saved.metadata=r.metadata;
            }
            a->onSnapshotAt(product,t,local,levels); if(b) b->onSnapshotAt(product,t,local,std::move(levels));
        };
        feed.onUpdates=[&](int64_t t,int64_t local,std::vector<Level> levels) {
            if(a) a->onUpdatesAt(product,t,local,levels); if(b) b->onUpdatesAt(product,t,local,std::move(levels));
        };
        feed.onInvalid=[&](int64_t t,const std::string& reason) { if(a) a->onInvalid(product,t,reason); if(b) b->onInvalid(product,t,reason); };
        feed.onTick=[&](int64_t t) { if(a) a->onTick(t); if(b) b->onTick(t); };
        while(reader.next(r)) {
            feed.apply(r);
            const auto local=r.record.time.systemNs/1'000'000;
            if(a && !b && local>=boundary) {
                saved.recorder.bytes=a->exportState(); saved.boundaryMs=boundary; saved.pos=r.pos;
                AnchorStore::write(root/"anchors",saved);
                auto loaded=AnchorStore::read(AnchorStore::path(root/"anchors",saved),saved.identity,boundary,saved.pos);
                ASSERT_TRUE(loaded);
                copyPrefix(root/"a",root/"b"); cfg.root=root/"b";
                b=std::make_unique<BookRecorder>(cfg); b->importState(loaded->recorder.bytes);
                EXPECT_EQ(saved.recorder.bytes,b->exportState()); sameWatermarks(*a,*b,product);
                std::cout << "ANCHOR_EXPORT product=" << product << " recorder_bytes=" << saved.recorder.bytes.size()
                          << " sidecar_bytes=" << fs::file_size(AnchorStore::path(root/"anchors",saved)) << '\n';
            } else if(b) ++suffix;
            if(a && local>=end) break;
        }
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_GT(suffix,0);
        ASSERT_GE(r.record.time.systemNs/1'000'000-first,3'600'000);
        a->drain(); b->drain(); sameWatermarks(*a,*b,product);
        EXPECT_EQ(a->exportState(),b->exportState());
        const auto fa=files(root/"a"),fb=files(root/"b"); ASSERT_FALSE(fa.empty()); EXPECT_TRUE(fa==fb);
        const auto hours=Hmc2Store::readRange(root/"b",product,"deep",3'600'000,first/3'600'000*3'600'000,end);
        ASSERT_FALSE(hours.empty());
        std::cout << "ANCHOR_PARITY product=" << product << " suffix_records=" << suffix
                  << " files=" << fa.size() << " hours=" << hours.size() << " byte_diff=0 watermarks=identical\n";
    }
}

TEST(AnchorStore, RetentionKeepsMidnightAndTouchesOnlyExpiredIntraday) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    auto a=anchor(); AnchorStore::write(root,a); const auto midnight=AnchorStore::path(root,a);
    a.boundaryMs+=Quarter; AnchorStore::write(root,a); const auto old=AnchorStore::path(root,a);
    a.identity.dayMs+=Day; a.identity.fromMs+=Day; a.identity.toMs+=Day; a.boundaryMs+=Day;
    AnchorStore::write(root,a); const auto recent=AnchorStore::path(root,a);
    const auto unrelated=old.parent_path()/"journal.rawl2"; std::ofstream(unrelated)<<"untouched";
    AnchorStore::prune(root,"BTC-USD",Epoch+2*Day+2*Quarter);
    EXPECT_TRUE(fs::exists(midnight)); EXPECT_FALSE(fs::exists(old)); EXPECT_TRUE(fs::exists(recent));
    EXPECT_TRUE(fs::exists(unrelated));
}
TEST(FeedState, ImportHasNoCallbacksAndPreservesSequenceAndInvalidation) {
    JournalFeed a("BTC-USD"),b("BTC-USD");
    json j={{"version",1},{"product","BTC-USD"},{"run","run"},{"connection",7},
        {"lastLocal",Epoch},{"sequence",17},{"anchored",true}};
    int invalid=0,ticks=0,snapshots=0;
    b.onInvalid=[&](auto,const auto&){++invalid;}; b.onTick=[&](auto){++ticks;};
    b.onSnapshot=[&](auto,auto,auto){++snapshots;};
    a.importState(j); b.importState(a.exportState());
    EXPECT_EQ(j,b.exportState()); EXPECT_EQ(invalid+ticks+snapshots,0);
    JournalRecord r{{capture::Kind::Frame,{(Epoch+1000)*1000000,0},7,
        R"({"channel":"heartbeats","sequence_num":18})"},{"BTC-USD","run",4,2},meta(),false,1};
    b.apply(r); EXPECT_EQ(invalid,0); EXPECT_EQ(ticks,1);
    b.apply(r); EXPECT_EQ(invalid,1); EXPECT_FALSE(b.exportState()["anchored"].get<bool>());
    const auto before=b.exportState(); j["product"]="PEPE-USD";
    EXPECT_THROW(b.importState(j),std::runtime_error); EXPECT_EQ(before,b.exportState());
}
namespace {
std::string bookFrame(const std::string& product, bool snapshot, int i) {
    return json({{"channel","l2_data"},{"sequence_num",i+1},
        {"events",json::array({{{"type",snapshot?"snapshot":"update"},{"product_id",product},
        {"updates",json::array({{{"side","bid"},{"price_level","99999"},{"new_quantity",std::to_string(1+i%7)}},
                              {{"side","offer"},{"price_level","100001"},{"new_quantity","3.12345678901234"}}})}}})}}).dump();
}
void replayFixture(const fs::path& root, int64_t duration=2*3'600'000, int64_t first=0, bool resnapshot=false) {
    capture::WriterConfig cfg; cfg.root=QString::fromStdString(root.string()); cfg.fsyncBlocks=0;
    capture::Writer w(cfg,{{"product_metadata",meta()}});
    // Original grid snapshot is in the previous day's file. Today has no snapshot.
    w.append({capture::Kind::Frame,{(Epoch-1000)*1000000,0},1,bookFrame("BTC-USD",true,0)});
    for (int64_t t=first;t<=duration+5000;t+=1000)
        w.append({capture::Kind::Frame,{(Epoch+t)*1000000,(t+1000)*1000000},1,bookFrame("BTC-USD",resnapshot && t==45*Minute,int((t-first)/1000)+1)});
    w.close();
}
void parity(const fs::path& a,const fs::path& b,const std::string& product,int64_t end) {
    EXPECT_TRUE(files(a)==files(b)) << "HMC2 bytes";
    for (const auto* layer:{"near","deep"}) for (int64_t tf:{Minute,int64_t(3'600'000)})
        EXPECT_EQ(diff(a,b,product,layer,Epoch,end,tf,true).at("mismatching"),0) << layer << '/' << tf;
}
}
TEST(AnchorReplay, EveryQuarterBoundedResumeAndIndependentMidnight) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); replayFixture(root/"raw",2*3'600'000+Minute);
    RollOptions full{root/"raw",root/"full","BTC-USD",Epoch,Epoch+2*3'600'000};
    int64_t lastFull=0; full.onApplied=[&](const auto& r){lastFull=r.record.time.systemNs/1000000;};
    const auto report=roll(full);
    EXPECT_LE(lastFull,full.toMs+5000);
    ASSERT_EQ(AnchorStore::candidates(root/"full","BTC-USD",Epoch).size(),8);
    // Copy only today's journal, excluding the snapshot in yesterday's segment.
    JournalReader inventory(root/"raw","BTC-USD");
    fs::create_directories(root/"today");
    for (const auto& file:inventory.files()) if(file.header.at("opened_system_ns").get<int64_t>()/1000000>=Epoch)
        fs::copy_file(file.path,root/"today"/file.path.filename());
    auto resumed=full; resumed.journalRoot=root/"today"; resumed.outputRoot=root/"resumed";
    resumed.anchorRoot=full.outputRoot; resumed.writeAnchors=false;
    for (int quarter=0;quarter<8;++quarter) {
        bool stop=false; int64_t last=0;
        const auto boundary=Epoch+(quarter+1)*Quarter;
        resumed.cancelled=[&]{return stop;};
        resumed.onApplied=[&](const auto& r) { last=r.record.time.systemNs/1000000; if(last>=boundary) stop=true; };
        const auto chunk=roll(resumed);
        ASSERT_TRUE(chunk["days"][0].contains("anchorBoundaryMs")) << chunk.dump();
        EXPECT_EQ(chunk["days"][0]["anchorBoundaryMs"],Epoch+quarter*Quarter) << chunk.dump();
        EXPECT_LE(chunk.at("decodedRecords").get<uint64_t>(),950u);
        EXPECT_EQ(chunk["days"][0]["anchorCandidatesDecoded"],1);
        if (quarter==7) { resumed.cancelled={}; resumed.onApplied={}; roll(resumed); }
    }
    parity(full.outputRoot,resumed.outputRoot,"BTC-USD",full.toMs);
    std::cout<<"ANCHOR_SYNTHETIC every_quarter=8 decoded_bound=950 midnight_independent=true hmc2_diff=0\n";
}
TEST(AnchorReplay, CorruptMissingAndAheadCandidatesFallbackIdentically) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); replayFixture(root/"raw");
    RollOptions baseline{root/"raw",root/"full","BTC-USD",Epoch,Epoch+2*3'600'000}; roll(baseline);
    const auto candidates=AnchorStore::candidates(root/"full","BTC-USD",Epoch);
    const auto original=AnchorStore::load(candidates.back());
    for (const auto* mode:{"truncated","crc","policy","missing-midnight","ahead","feed","recorder"}) {
        auto o=baseline; o.outputRoot=root/mode; o.writeAnchors=false;
        auto a=original;
        if(std::string_view(mode)=="policy") ++a.identity.configHash;
        if(std::string_view(mode)=="ahead") a.pos.block+=100000;
        if(std::string_view(mode)=="feed") a.feed.bytes={0};
        if(std::string_view(mode)=="recorder") a.recorder.bytes.back()^=1;
        if(std::string_view(mode)!="missing-midnight") {
            AnchorStore::write(o.outputRoot,a); const auto path=AnchorStore::path(o.outputRoot,a);
            if(std::string_view(mode)=="truncated") fs::resize_file(path,20);
            if(std::string_view(mode)=="crc") {
                auto bytes=AnchorStore::encode(a); bytes.back()^=1;
                std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            }
        }
        const auto report=roll(o);
        EXPECT_FALSE(report["days"][0].contains("anchorBoundaryMs")) << mode;
        parity(baseline.outputRoot,o.outputRoot,"BTC-USD",o.toMs);
    }
}
namespace {
bool sameHmcBytes(const fs::path& a,const fs::path& b) {
    std::map<std::string,uint64_t> left,right;
    const auto inventory=[](const fs::path& root,auto& out) {
        for(const auto& e:fs::recursive_directory_iterator(root)) if(e.path().extension()==".hmc2")
            out[fs::relative(e.path(),root).string()]=e.file_size();
    };
    inventory(a,left); inventory(b,right);
    if(left!=right) return false;
    for(const auto& [name,size]:left) {
        std::ifstream x(a/name,std::ios::binary),y(b/name,std::ios::binary);
        std::array<char,65536> xb,yb;
        while(x) {
            x.read(xb.data(),xb.size()); y.read(yb.data(),yb.size());
            if(x.gcount()!=y.gcount() || !std::equal(xb.begin(),xb.begin()+x.gcount(),yb.begin())) return false;
        }
    }
    return true;
}
}
// Explicit long, read-only captured-day acceptance, run through the queue.
// Every quarter is a separate roll() lifetime. The committed prefix is carried
// forward; all 96 imports together regenerate a complete day from its RAWL2.
TEST(AnchorReplay, RealDayEveryAnchorMidnightAndSevenProductBenchmark) {
    const char* input=std::getenv("SENTINEL_ANCHOR_DAY_ROOT");
    if(!input) GTEST_SKIP()<<"set SENTINEL_ANCHOR_DAY_ROOT for full real-day acceptance";
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    for(const auto* product:{"BTC-USD","PEPE-USD","ETH-USD","SOL-USD","DOGE-USD","AVAX-USD","FARTCOIN-USD"}) {
        const bool all=std::string_view(product)=="BTC-USD" || std::string_view(product)=="PEPE-USD";
        RollOptions full{input,root/"full"/product,product,Epoch,Epoch+Day};
        full.useAnchors=false;
        const auto baseline=roll(full);
        const auto anchors=AnchorStore::candidates(full.outputRoot,product,Epoch);
        ASSERT_EQ(anchors.size(),96) << product;
        uint64_t minSize=UINT64_MAX,maxSize=0;
        for(const auto& f:anchors) { minSize=std::min<uint64_t>(minSize,fs::file_size(f)); maxSize=std::max<uint64_t>(maxSize,fs::file_size(f)); }
        std::cout<<"ANCHOR_SIZE product="<<product<<" count=96 min="<<minSize<<" max="<<maxSize<<std::endl;
        if(all) {
            // A read-only source view contains only day N and the next day's
            // first hour (the real lateness fence). No day N-1 is present.
            const auto today=root/"today"/product;
            fs::create_directories(today);
            for(const auto* date:{"05","06"}) {
                const auto source=fs::path(input)/product/"2026"/"10"/date;
                for(const auto& f:fs::directory_iterator(source)) if(f.path().extension()==".rawl2" &&
                    (std::string_view(date)=="05" || f.path().filename()=="00.rawl2"))
                    fs::create_symlink(f.path(),today/(std::string(date)+"-"+f.path().filename().string()));
            }
            auto r=full; r.useAnchors=true; r.writeAnchors=false; r.journalRoot=today;
            r.outputRoot=root/"quarter"/product; r.anchorRoot=full.outputRoot;
            uint64_t total=0,maxDecoded=0; int64_t maxWindow=0;
            for(int quarter=0;quarter<96;++quarter) {
                bool stop=false; int64_t first=0,last=0;
                const auto boundary=Epoch+(quarter+1)*Quarter;
                r.cancelled=[&]{return stop;};
                r.onApplied=[&](const auto& record) {
                    last=record.record.time.systemNs/1000000; if(!first) first=last;
                    if(quarter<95 && last>=boundary) stop=true;
                };
                const auto report=roll(r);
                ASSERT_TRUE(report["days"][0].contains("anchorBoundaryMs"))<<product<<' '<<quarter<<' '<<report.dump();
                ASSERT_EQ(report["days"][0]["anchorBoundaryMs"],Epoch+quarter*Quarter)<<product;
                EXPECT_EQ(report["days"][0]["anchorCandidatesDecoded"],1)<<product<<' '<<quarter;
                const auto count=report.at("decodedRecords").get<uint64_t>();
                maxDecoded=std::max(maxDecoded,count); total+=count;
                maxWindow=std::max(maxWindow,last-first);
                EXPECT_LE(last-first,Quarter+10'000)<<product<<' '<<quarter;
                // Decoded includes the selected block's prefix. Never a whole
                // preceding segment/day (also independently tested at reader level).
                EXPECT_LT(count,baseline.at("decodedRecords").get<uint64_t>()/10)<<product<<' '<<quarter;
            }
            ASSERT_TRUE(sameHmcBytes(full.outputRoot,r.outputRoot))<<product;
            for(const auto* layer:{"near","deep"}) for(int64_t tf:{Minute,int64_t(3'600'000)})
                EXPECT_EQ(diff(full.outputRoot,r.outputRoot,product,layer,Epoch,Epoch+Day,tf,true)["mismatching"],0);
            std::cout<<"ANCHOR_REAL_DAY product="<<product<<" imports=96 max_decoded="<<maxDecoded
                <<" max_window_ms="<<maxWindow<<" total_decoded="<<total
                <<" midnight_independent=true byte_diff=0 hmc2_diff=0"<<std::endl;
        }
        // Restart at the final committed minute, with identical already-durable
        // HMC2 bytes for both runs. The baseline takes the exchange-snapshot path.
        const auto cpPath=full.outputRoot/product/"roller.json";
        json cp; {std::ifstream in(cpPath);in>>cp;}
        cp["days"][std::to_string(Epoch)]["committedThroughMs"]=Epoch+Day-Minute;
        cp["committedThroughMs"]=Epoch+Day-Minute;
        writeCheckpoint(cpPath,cp);
        auto restart=full; restart.writeAnchors=false;
        const auto before=roll(restart);
        writeCheckpoint(cpPath,cp); restart.useAnchors=true;
        const auto after=roll(restart);
        ASSERT_TRUE(after["days"][0].contains("anchorBoundaryMs"))<<after.dump();
        EXPECT_LT(after.at("wallSeconds").get<double>(),before.at("wallSeconds").get<double>());
        EXPECT_LT(after.at("decodedRecords").get<uint64_t>(),before.at("decodedRecords").get<uint64_t>()/10);
        std::cout<<"ANCHOR_RESTART product="<<product<<" before_s="<<before["wallSeconds"]
            <<" after_s="<<after["wallSeconds"]<<" before_decoded="<<before["decodedRecords"]
            <<" after_decoded="<<after["decodedRecords"]<<std::endl;
    }
}
TEST(AnchorReplay, AnchorAheadOfCheckpointOrHandshakeIsNotInstalled) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); replayFixture(root/"raw");
    RollOptions full{root/"raw",root/"full","BTC-USD",Epoch,Epoch+2*3'600'000}; roll(full);
    auto o=full; o.outputRoot=root/"ahead"; o.writeAnchors=false;
    copyPrefix(full.outputRoot,o.outputRoot);
    const auto candidates=AnchorStore::candidates(full.outputRoot,"BTC-USD",Epoch);
    auto a=AnchorStore::load(candidates[candidates.size()-2]);
    ASSERT_EQ(a.boundaryMs,Epoch+Quarter); AnchorStore::write(o.outputRoot,a);
    json cp; {std::ifstream in(full.outputRoot/"BTC-USD"/"roller.json");in>>cp;}
    JournalReader reader(root/"raw","BTC-USD"); JournalRecord r;
    while(reader.next(r)) if(r.record.time.systemNs/1000000>=Epoch+14*Minute) break;
    cp["days"][std::to_string(Epoch)]["pos"]=r.pos;
    cp["days"][std::to_string(Epoch)]["committedThroughMs"]=Epoch+13*Minute;
    writeCheckpoint(o.outputRoot/"BTC-USD"/"roller.json",cp);
    auto result=roll(o); EXPECT_FALSE(result["days"][0].contains("anchorBoundaryMs"));
    parity(full.outputRoot,o.outputRoot,"BTC-USD",o.toMs);
    o.outputRoot=root/"handshake"; o.anchorRoot=full.outputRoot;
    o.anchorAllowed=[](const auto&){return false;};
    int restores=0; o.onAnchorRestore=[&](const auto&,const auto&,bool,bool,int64_t){++restores;};
    result=roll(o); EXPECT_FALSE(result["days"][0].contains("anchorBoundaryMs")); EXPECT_EQ(restores,0);
    parity(full.outputRoot,o.outputRoot,"BTC-USD",o.toMs);
}
TEST(AnchorReplay, CliRebuildCreatesOnlySidecarsAndMatchesNormalExport) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); replayFixture(root/"raw");
    RollOptions full{root/"raw",root/"full","BTC-USD",Epoch,Epoch+2*3'600'000}; roll(full);
    std::ofstream(root/"config.yaml") << "recording:\n  dir: " << (root/"primary").string() << "\n";
    std::vector<std::string> args={"sentinel-roll","rebuild-anchors",(root/"raw").string(),(root/"rebuilt").string(),
        "--products","BTC-USD","--from","2026-10-05T00:00:00Z","--to","2026-10-05T02:00:00Z","--config",(root/"config.yaml").string()};
    std::vector<char*> argv; for(auto& s:args) argv.push_back(s.data());
    ASSERT_EQ(rollMain(int(argv.size()),argv.data()),0);
    EXPECT_TRUE(files(root/"rebuilt").empty());
    const auto normal=AnchorStore::candidates(full.outputRoot,"BTC-USD",Epoch);
    const auto rebuilt=AnchorStore::candidates(root/"rebuilt","BTC-USD",Epoch);
    ASSERT_EQ(normal.size(),rebuilt.size());
    for(size_t i=0;i<normal.size();++i)
        EXPECT_EQ(AnchorStore::encode(AnchorStore::load(normal[i])),AnchorStore::encode(AnchorStore::load(rebuilt[i])));
}
TEST(AnchorReplay, LateFirstRecordKeepsMidnightIndependent) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    replayFixture(root/"raw",2*3'600'000,5*Minute);
    RollOptions full{root/"raw",root/"full","BTC-USD",Epoch,Epoch+2*3'600'000}; roll(full);
    const auto a=AnchorStore::load(AnchorStore::candidates(full.outputRoot,"BTC-USD",Epoch).back());
    EXPECT_LT(json::from_cbor(a.feed.bytes)["feed"]["lastLocal"],Epoch);
    JournalReader inventory(root/"raw","BTC-USD"); fs::create_directories(root/"today");
    for(const auto& f:inventory.files()) if(f.header.at("opened_system_ns").get<int64_t>()/1000000>=Epoch)
        fs::copy_file(f.path,root/"today"/f.path.filename());
    auto o=full; o.outputRoot=root/"independent"; o.journalRoot=root/"today";
    o.anchorRoot=full.outputRoot; o.writeAnchors=false;
    const auto report=roll(o); ASSERT_TRUE(report["days"][0].contains("anchorBoundaryMs"));
    parity(full.outputRoot,o.outputRoot,"BTC-USD",o.toMs);
    EXPECT_EQ(Hmc2Store::readRange(o.outputRoot,"BTC-USD","near",Minute,Epoch,Epoch+5*Minute).size(),5);
}
TEST(AnchorReplay, DamagedCommittedOverlapUsesSnapshotFallback) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString();
    replayFixture(root/"raw",2*3'600'000,0,true);
    RollOptions o{root/"raw",root/"resume","BTC-USD",Epoch,Epoch+2*3'600'000};
    o.productWriterLease=true;
    bool stop=false; o.cancelled=[&]{return stop;};
    o.onApplied=[&](const auto& r){ if(r.record.time.systemNs/1000000>=Epoch+40*Minute) stop=true; };
    roll(o); o.cancelled={}; o.onApplied={}; o.writeAnchors=false;
    fs::copy(o.outputRoot,root/"reference",fs::copy_options::recursive);
    JournalReader inventory(root/"raw","BTC-USD"); bool damaged=false;
    for(const auto& f:inventory.files()) {
        const auto scan=capture::scan(QString::fromStdString(f.path.string()));
        for(const auto& block:scan.index) if(block.firstSystemNs/1000000>=Epoch+35*Minute &&
                                           block.firstSystemNs/1000000<Epoch+35*Minute+2000) {
            std::fstream out(f.path,std::ios::binary|std::ios::in|std::ios::out);
            out.seekg(block.offset+48); char byte; out.read(&byte,1); byte^=1;
            out.seekp(block.offset+48); out.write(&byte,1); damaged=true; break;
        }
        if(damaged) break;
    }
    ASSERT_TRUE(damaged);
    auto reference=o; reference.outputRoot=root/"reference"; reference.useAnchors=false; roll(reference);
    json resumed;
    ASSERT_NO_THROW(resumed=roll(o));
    EXPECT_FALSE(resumed["days"][0].contains("anchorBoundaryMs"));
    EXPECT_EQ(resumed["days"][0]["committedThroughMs"],o.toMs);
    parity(reference.outputRoot,o.outputRoot,"BTC-USD",o.toMs);
}
TEST(AnchorReplay, CancelledRangeDoesNotPruneFutureTime) {
    QTemporaryDir tmp; const fs::path root=tmp.path().toStdString(); replayFixture(root/"raw");
    RollOptions o{root/"raw",root/"out","BTC-USD",Epoch,Epoch+2*3'600'000};
    auto a=anchor(); a.identity.dayMs-=2*Day; a.identity.fromMs-=2*Day; a.identity.toMs-=2*Day;
    a.boundaryMs=Epoch-2*Day+2*Quarter; AnchorStore::write(o.outputRoot,a);
    const auto young=AnchorStore::path(o.outputRoot,a);
    a.identity.dayMs-=Day; a.identity.fromMs-=Day; a.identity.toMs-=Day;
    a.boundaryMs=Epoch-2*Day-Quarter; AnchorStore::write(o.outputRoot,a);
    const auto expired=AnchorStore::path(o.outputRoot,a);
    bool stop=false; o.cancelled=[&]{return stop;};
    o.onApplied=[&](const auto& r){ if(r.record.time.systemNs/1000000>=Epoch+Quarter) stop=true; };
    roll(o);
    EXPECT_TRUE(fs::exists(young)); EXPECT_FALSE(fs::exists(expired));
}
