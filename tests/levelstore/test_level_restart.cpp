#include "LevelTestSupport.hpp"
#include "RestartJournal.hpp"
#include "Crc32.hpp"
#include "roller/LevelCheckpoint.hpp"
#include "roller/Roller.hpp"
#include "roller/AnchorStore.hpp"
#include "levelstore/LevelLog.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <map>
#include <atomic>
#include <zstd.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
extern char** environ;
#endif
using namespace sentinel;
using namespace sentinel::roller;
using namespace levelstore_test;
using nlohmann::json;
namespace {
constexpr auto Epoch = RestartJournal::Epoch;
constexpr int64_t Minute = 60'000;
std::string executable;
std::string contents(const fs::path& p) {
    std::ifstream in(p,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};
}
std::map<std::string,std::string> tree(const fs::path& p, const char* extension) {
    std::map<std::string,std::string> result;
    for (const auto& f : fs::recursive_directory_iterator(p))
        if (f.is_regular_file() && f.path().extension()==extension && !f.path().filename().string().starts_with("._"))
            result[fs::relative(f.path(),p).string()]=contents(f.path());
    return result;
}
json checkedRoll(RollOptions o) {
    recording::BookRecorder* recorder=nullptr;
    LevelRecorder* level=nullptr;
    std::vector<uint8_t> finalState;
    const auto recorderHook=o.onRecorder;
    const auto levelHook=o.onLevelRecorder;
    o.onRecorder=[&](auto* p,auto* feed,int64_t end) {
        recorder=p; if (recorderHook) recorderHook(p,feed,end);
    };
    o.onLevelRecorder=[&](auto* p) {
        if (!p && level && recorder) {
            try {
                const auto bytes=recorder->exportState();
                finalState=level->exportState(recorder->checkpointClock(o.product),bytes);
            } catch (...) { finalState.clear(); }
        }
        level=p; if (levelHook) levelHook(p);
    };
    auto result=sentinel::roller::roll(o);
    if (!o.levelLogRoot.empty()) {
        if (finalState.empty()) throw std::runtime_error("successful roll has no live final C state");
        std::ofstream out(o.outputRoot.parent_path()/"final.state",std::ios::binary);
        out.write(reinterpret_cast<const char*>(finalState.data()),finalState.size());
    }
    return result;
}
RollOptions options(const fs::path& raw, const fs::path& root) {
    RollOptions o{raw,root/"hmc","BTC-USD",Epoch,Epoch+90*Minute};
    o.levelLogRoot=root/"log"; return o;
}
json state(const fs::path& root) {
    return json::parse(contents(root/"hmc/BTC-USD/roller.json"))["days"][std::to_string(Epoch)]["level"];
}
void equal(const fs::path& a, const fs::path& b) {
    EXPECT_EQ(tree(a/"log",".lcl"),tree(b/"log",".lcl"));
    EXPECT_EQ(tree(a/"hmc",".hmc2"),tree(b/"hmc",".hmc2"));
    ASSERT_TRUE(state(a).is_object());
    ASSERT_TRUE(state(b).is_object());
    EXPECT_EQ(state(a),state(b));
    EXPECT_EQ(contents(a/"final.state"),contents(b/"final.state"));
    EXPECT_FALSE(tree(a/"hmc",".hmc2").empty());
}
struct FaultFiles : LevelLogFiles {
    std::atomic<bool> armed{false};
    bool killWrite=false, killCut=false;
    size_t write(std::FILE* f,std::span<const uint8_t> b) override {
        if (armed.exchange(false)) {
            LevelLogFiles::write(f,b.first(std::max(size_t(1),b.size()/2)));
#ifndef _WIN32
            if (killWrite) ::kill(::getpid(),SIGKILL);
#endif
            throw std::runtime_error("injected C append failure after side effect");
        }
        return LevelLogFiles::write(f,b);
    }
    void truncate(const fs::path& p,uint64_t size) override {
        LevelLogFiles::truncate(p,size);
#ifndef _WIN32
        if (killCut) ::kill(::getpid(),SIGKILL);
#endif
    }
};
}
TEST(LevelRestart, GrowingJournalMatchesOneRoll) {
    TempDir t; RestartJournal journal(t.path/"raw");
    auto stepped=options(t.path/"raw",t.path/"stepped");
    for (int m : {20,40,60,90}) { journal.grow(m); ASSERT_NO_THROW(checkedRoll(stepped)); }
    ASSERT_NO_THROW(checkedRoll(options(t.path/"raw",t.path/"one")));
    equal(t.path/"one",t.path/"stepped");
    auto baseline=options(t.path/"raw",t.path/"baseline"); baseline.levelLogRoot.clear();
    ASSERT_NO_THROW(checkedRoll(baseline));
    EXPECT_EQ(tree(baseline.outputRoot,".hmc2"),tree(t.path/"one/hmc",".hmc2"));
}
TEST(LevelRestart, RetryAfterFailedCUsesProcessRestartPath) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    ASSERT_NO_THROW(checkedRoll(options(t.path/"raw",t.path/"one")));
    auto o=options(t.path/"raw",t.path/"retry");
    auto fault=std::make_shared<FaultFiles>(); o.levelLogFiles=fault;
    o.onApplied=[&](const JournalRecord& r) { if (r.record.time.systemNs/1'000'000>=Epoch+35*Minute) fault->armed=true; };
    EXPECT_THROW(checkedRoll(o),std::exception);
    o.onApplied={}; o.levelLogFiles={};
    ASSERT_NO_THROW(checkedRoll(o)); equal(t.path/"one",t.path/"retry");
}
TEST(LevelRestart, HmcOnlyResumeRefusesBeforeOutput) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); o.levelLogRoot.clear(); checkedRoll(o);
    o.levelLogRoot=t.path/"log";
    EXPECT_THROW(sentinel::roller::roll(o),std::exception); EXPECT_FALSE(fs::exists(o.levelLogRoot));
    fs::remove(o.outputRoot/"BTC-USD/roller.json");
    // Keep anchors but avoid the independent HMC2 provenance check.
    o.anchorRoot=o.outputRoot; o.outputRoot=t.path/"fresh";
    EXPECT_THROW(sentinel::roller::roll(o),std::exception); EXPECT_FALSE(fs::exists(o.levelLogRoot));
}
TEST(LevelRestart, PairedCheckpointMismatchRefusesBeforeCut) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); checkedRoll(o);
    const auto path=o.outputRoot/"BTC-USD/roller.json";
    const auto original=contents(path); const auto before=tree(o.levelLogRoot,".lcl");
    for (bool length : {false,true}) {
        auto cp=json::parse(original);
        auto& digest=cp["days"][std::to_string(Epoch)]["level"]["anchor"]["recorder"];
        digest[length ? "length" : "crc"]=digest[length ? "length" : "crc"].get<uint64_t>()+1;
        std::ofstream(path)<<cp.dump();
        try { checkedRoll(o); FAIL()<<"accepted mismatched recorder pair"; }
        catch (const std::exception& e) { EXPECT_NE(std::string(e.what()).find("not paired"),std::string::npos)<<e.what(); }
        EXPECT_EQ(tree(o.levelLogRoot,".lcl"),before);
    }
}
#ifndef _WIN32
TEST(LevelRestart, KillAndRestartMatchesAllBytes) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    ASSERT_NO_THROW(checkedRoll(options(t.path/"raw",t.path/"one")));
    for (const std::string point : {"initial-append","append","checkpoint-synced","checkpoint-mid-write","anchor-synced","anchor-mid-write","cut"}) {
        SCOPED_TRACE(point); const auto root=t.path/point;
        const auto spawn = [&](const std::string& mode) {
            std::vector<std::string> args={executable,"--restart-child",(t.path/"raw").string(),root.string(),mode};
            std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
            pid_t child=-1;
            const auto error=::posix_spawn(&child,executable.c_str(),nullptr,nullptr,argv.data(),environ);
            EXPECT_EQ(error,0);
            if (error) return -1;
            int status=0;
            EXPECT_EQ(::waitpid(child,&status,0),child);
            EXPECT_TRUE(WIFSIGNALED(status));
            return WIFSIGNALED(status) ? WTERMSIG(status) : -1;
        };
        ASSERT_EQ(spawn(point),SIGKILL);
        if (point=="cut") ASSERT_EQ(spawn("mid-cut"),SIGKILL);
        ASSERT_NO_THROW(checkedRoll(options(t.path/"raw",root))); equal(t.path/"one",root);
    }
}
#endif

TEST(LevelRestart, GrowingRangeAndBeforeFirstQuarterMatchOneRoll) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"stepped"); o.allowGrowingEnd=true;
    for (int m : {15,30,60,90}) { o.toMs=Epoch+m*Minute; ASSERT_NO_THROW(checkedRoll(o)); }
    auto one=options(t.path/"raw",t.path/"one"); one.allowGrowingEnd=true; checkedRoll(one);
    equal(t.path/"one",t.path/"stepped");
    auto shortRoll=options(t.path/"raw",t.path/"short"); shortRoll.fromMs=Epoch+7*Minute; shortRoll.toMs=Epoch+9*Minute;
    checkedRoll(shortRoll); const auto before=tree(shortRoll.levelLogRoot,".lcl");
    ASSERT_NO_THROW(checkedRoll(shortRoll)); EXPECT_EQ(tree(shortRoll.levelLogRoot,".lcl"),before);
}
TEST(LevelRestart, PairedAnchorMismatchRefusesBeforeCut) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); checkedRoll(o);
    const auto candidates=AnchorStore::candidates(o.outputRoot,"BTC-USD",Epoch);
    ASSERT_FALSE(candidates.empty());
    auto a=AnchorStore::load(candidates.front());
    auto j=json::from_cbor(a.recorder.bytes.begin(),a.recorder.bytes.end()-4);
    j["stats"][1]=1; a.recorder.bytes=json::to_cbor(j);
    const auto crc=sentinel::crc32(a.recorder.bytes.data(),a.recorder.bytes.size());
    for (int i=0;i<4;++i) a.recorder.bytes.push_back(uint8_t(crc>>(8*i)));
    AnchorStore::write(o.outputRoot,a);
    const auto before=tree(o.levelLogRoot,".lcl");
    EXPECT_THROW(checkedRoll(o),std::exception); EXPECT_EQ(tree(o.levelLogRoot,".lcl"),before);
}

TEST(LevelRestart, LegacyAnchorEnvelopeRemainsReadableWithoutC) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"old"); o.levelLogRoot.clear(); checkedRoll(o);
    const auto path=AnchorStore::candidates(o.outputRoot,"BTC-USD",Epoch).front();
    auto encoded=AnchorStore::encode(AnchorStore::load(path));
    const auto word=[&](size_t at) { uint32_t n=0; for (int i=0;i<4;++i) n|=uint32_t(encoded[at+i])<<(8*i); return n; };
    std::vector<uint8_t> raw(word(12));
    ASSERT_EQ(ZSTD_decompress(raw.data(),raw.size(),encoded.data()+20,word(16)),raw.size());
    auto payload=json::from_cbor(raw); payload.erase("level"); raw=json::to_cbor(payload);
    std::vector<uint8_t> packed(ZSTD_compressBound(raw.size()));
    const auto size=ZSTD_compress(packed.data(),packed.size(),raw.data(),raw.size(),3);
    ASSERT_FALSE(ZSTD_isError(size));
    encoded.resize(8);
    const auto put=[&](uint32_t n) { for (int i=0;i<4;++i) encoded.push_back(uint8_t(n>>(8*i))); };
    put(1); put(uint32_t(raw.size())); put(uint32_t(size));
    encoded.insert(encoded.end(),packed.begin(),packed.begin()+size);
    put(sentinel::crc32(encoded.data(),encoded.size()));
    EXPECT_TRUE(AnchorStore::decode(encoded).level.is_null());
    std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(encoded.data()),encoded.size()); out.close();
    ASSERT_NO_THROW(checkedRoll(o));
}

TEST(LevelRestart, EmptyAndImageFencesDiscardUnverifiedSuffixBeforeOpen) {
    for (bool warmup : {false,true}) {
        SCOPED_TRACE(warmup); TempDir t; RestartJournal journal(t.path/"raw",warmup); journal.grow(90);
        checkedRoll(options(t.path/"raw",t.path/"one"));
        auto o=options(t.path/"raw",t.path/"restart");
        o.afterAnchorForTest=[](int64_t boundary) { if (boundary==Epoch) throw std::runtime_error("stop at first anchor"); };
        ASSERT_THROW(checkedRoll(o),std::exception);
        const auto file=LevelLogReader::hourPath(o.levelLogRoot,"BTC-USD",Epoch);
        fs::create_directories(file.parent_path());
        std::ofstream broken(file,std::ios::binary|std::ios::app); broken<<std::string(256,'\0'); broken.close();
        o.afterAnchorForTest={}; ASSERT_NO_THROW(checkedRoll(o)); equal(t.path/"one",t.path/"restart");
    }
}

TEST(LevelRestart, CompletedCheckpointDoesNotConsumePastItsPosition) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(100);
    auto o=options(t.path/"raw",t.path/"attempt"); o.allowGrowingEnd=true;
    ASSERT_NO_THROW(checkedRoll(o));
    const auto original=state(t.path/"attempt");
    const auto live=contents(t.path/"attempt/final.state");
    ASSERT_NO_THROW(checkedRoll(o));
    EXPECT_EQ(state(t.path/"attempt"),original);
    EXPECT_EQ(contents(t.path/"attempt/final.state"),live);
}

TEST(LevelRestart, CompactCheckpointReferencesBinaryAnchor) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"one"); checkedRoll(o);
    const auto bytes=contents(o.outputRoot/"BTC-USD/roller.json");
    const auto cp=json::parse(bytes); const auto& day=cp.at("days").at(std::to_string(Epoch));
    EXPECT_LT(bytes.size(),4096u);
    EXPECT_FALSE(day.contains("recorder")); EXPECT_FALSE(day.at("level").contains("state"));
    const auto a=AnchorStore::load(AnchorStore::candidates(o.outputRoot,"BTC-USD",Epoch).front());
    ASSERT_TRUE(a.level.at("state").is_binary());
    EXPECT_EQ(day.at("level").at("anchor"),anchorPair(a));
}
TEST(LevelRestart, MultidayRangeRefusesBeforeChangingOutput) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(24*60+20);
    auto o=options(t.path/"raw",t.path/"attempt"); o.toMs=Epoch+2*86'400'000;
    auto refuse=[&] {
        try { sentinel::roller::roll(o); FAIL()<<"accepted multi-day C range"; }
        catch (const std::exception& e) { EXPECT_NE(std::string(e.what()).find("one UTC day"),std::string::npos)<<e.what(); }
    };
    refuse(); EXPECT_FALSE(fs::exists(o.levelLogRoot)); EXPECT_FALSE(fs::exists(o.outputRoot));
    auto daily=o; daily.toMs=Epoch+86'400'000; checkedRoll(daily);
    daily.fromMs=daily.toMs; daily.toMs+=86'400'000; checkedRoll(daily);
    const auto logs=tree(o.levelLogRoot,".lcl"), hmc=tree(o.outputRoot,".hmc2");
    const auto cp=contents(o.outputRoot/"BTC-USD/roller.json");
    refuse(); EXPECT_EQ(tree(o.levelLogRoot,".lcl"),logs); EXPECT_EQ(tree(o.outputRoot,".hmc2"),hmc);
    EXPECT_EQ(contents(o.outputRoot/"BTC-USD/roller.json"),cp);
}
TEST(LevelRestart, ReplayedCheckpointValidatesClockAndTail) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(40);
    auto o=options(t.path/"raw",t.path/"attempt"); checkedRoll(o);
    const auto path=o.outputRoot/"BTC-USD/roller.json"; const auto original=contents(path);
    for (const char* field : {"clock","tail"}) {
        auto cp=json::parse(original); auto& c=cp["days"][std::to_string(Epoch)]["level"];
        if (std::string_view(field)=="clock") c["clock"]=c["clock"].get<int64_t>()+1;
        else c["tail"]["minute"]=c["tail"]["minute"].get<int64_t>()-Minute;
        std::ofstream(path)<<cp.dump(); EXPECT_THROW(checkedRoll(o),std::exception);
    }
}
TEST(LevelRestart, CancellationDuringCheckpointReplayReturnsCleanly) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(40);
    auto o=options(t.path/"raw",t.path/"attempt"); checkedRoll(o);
    const auto cp=contents(o.outputRoot/"BTC-USD/roller.json"); bool cancelled=false;
    o.onApplied=[&](const JournalRecord&) { cancelled=true; }; o.cancelled=[&] { return cancelled; };
    EXPECT_NO_THROW(sentinel::roller::roll(o)); EXPECT_TRUE(cancelled);
    EXPECT_EQ(contents(o.outputRoot/"BTC-USD/roller.json"),cp);
    o.onApplied={}; o.cancelled={}; journal.grow(90); checkedRoll(o);
    checkedRoll(options(t.path/"raw",t.path/"one")); equal(t.path/"one",t.path/"attempt");
}
TEST(LevelRestart, IgnoresUnusedDaysAndFutureCandidates) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(9);
    auto o=options(t.path/"raw",t.path/"attempt"); o.fromMs+=7*Minute; o.toMs=Epoch+9*Minute; checkedRoll(o);
    const auto path=o.outputRoot/"BTC-USD/roller.json";
    auto cp=json::parse(contents(path)); cp["days"][std::to_string(Epoch-2*86'400'000)]={{"committedThroughMs",Epoch-86'400'000}};
    // A newer preceding day with an empty fence is all this replay needs.
    cp["days"][std::to_string(Epoch-86'400'000)]={{"committedThroughMs",Epoch},{"level",{{"tail",nullptr}}}};
    std::ofstream(path)<<cp.dump();
    const auto anchors=o.outputRoot/"BTC-USD/anchors/2027-01-01"; fs::create_directories(anchors);
    std::ofstream(anchors/"2345.anchor")<<"unused future corrupt anchor";
    EXPECT_NO_THROW(checkedRoll(o));
}
TEST(LevelRestart, HmcOnlyReportsDrainAndRecoveryTiming) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(40);
    auto o=options(t.path/"raw",t.path/"attempt"); o.levelLogRoot.clear(); checkedRoll(o);
    unsigned drains=0, recoveries=0;
    o.levelTiming=[&](const char* name,double) { if (std::string_view(name)=="worker-drain") ++drains; };
    o.persistencePointForTest=[&](const char* name) { if (std::string_view(name)=="recovery-replayed") ++recoveries; };
    journal.grow(90); EXPECT_NO_THROW(checkedRoll(o)); EXPECT_GT(drains,0u); EXPECT_EQ(recoveries,1u);
}
TEST(LevelRestart, GrowingUnalignedJournalAcrossDayBoundary) {
    TempDir t; RestartJournal journal(t.path/"raw");
    auto o=options(t.path/"raw",t.path/"stepped"); o.allowGrowingEnd=true; o.toMs=Epoch+86'400'000;
    journal.grow(1439); checkedRoll(o); // EOF is 23:59:05, not a minute or day boundary.
    journal.grow(1440); checkedRoll(o);
    auto one=options(t.path/"raw",t.path/"one"); one.allowGrowingEnd=true; one.toMs=o.toMs; checkedRoll(one);
    equal(t.path/"one",t.path/"stepped");
    const auto dayOneLog=tree(o.levelLogRoot,".lcl");
    o.fromMs=o.toMs; o.toMs+=86'400'000;
    journal.grow(1460); checkedRoll(o); journal.grow(1480); checkedRoll(o);
    const auto bothDays=tree(o.levelLogRoot,".lcl");
    for (const auto& [file,bytes] : dayOneLog) {
        ASSERT_TRUE(bothDays.contains(file))<<file; EXPECT_EQ(bothDays.at(file),bytes)<<file;
    }
    one.fromMs=o.fromMs; one.toMs=o.toMs; checkedRoll(one);
    equal(t.path/"one",t.path/"stepped");
    checkedRoll(o); equal(t.path/"one",t.path/"stepped");
}

TEST(LevelRestart, PairDigestDistinguishesEqualLengthChecksummedPayloads) {
    auto checked=[](std::vector<uint8_t> bytes) {
        const auto crc=sentinel::crc32(bytes.data(),bytes.size());
        for (int i=0;i<4;++i) bytes.push_back(uint8_t(crc>>(8*i)));
        return bytes;
    };
    const auto a=checked({1,2,3}), b=checked({4,5,6});
    ASSERT_EQ(a.size(),b.size());
    EXPECT_EQ(pairDigest(a).at("length"),a.size());
    EXPECT_NE(pairDigest(a).at("crc"),pairDigest(b).at("crc"));
    EXPECT_EQ(pairDigest(a).at("crc"),sentinel::crc32(a.data(),a.size()-4));
}

TEST(LevelRestart, HmcOnlyCancellationRetainsDayProgress) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); o.levelLogRoot.clear(); bool stop=false;
    o.onApplied=[&](const JournalRecord& r) { stop=r.record.time.systemNs/1'000'000>=Epoch+35*Minute; };
    o.cancelled=[&] { return stop; };
    const auto report=checkedRoll(o);
    ASSERT_TRUE(stop); ASSERT_EQ(report.at("days").size(),1u);
    EXPECT_GT(report.at("days")[0].at("committedThroughMs").get<int64_t>(),Epoch);
}

int main(int argc,char** argv) {
    executable=fs::absolute(argv[0]).string();
#ifndef _WIN32
    if (argc==5 && std::string_view(argv[1])=="--restart-child") {
        try {
            const std::string point=argv[4];
            auto o=options(argv[2],argv[3]); auto f=std::make_shared<FaultFiles>(); o.levelLogFiles=f;
            bool ready=false; f->killWrite=true; f->killCut=point=="mid-cut";
            o.onApplied=[&](const JournalRecord& r) {
                ready=r.record.time.systemNs/1'000'000>=Epoch+(point=="initial-append" ? 0 : 35*Minute);
                if (ready && (point=="initial-append" || point=="append" || point=="cut")) f->armed=true;
            };
            o.persistencePointForTest=[&](const char* p) { if (ready && point==p) ::kill(::getpid(),SIGKILL); };
            checkedRoll(o); return 3;
        } catch (const std::exception& e) { std::fprintf(stderr,"restart child: %s\n",e.what()); return 4; }
    }
#endif
    ::testing::InitGoogleTest(&argc,argv);
    return RUN_ALL_TESTS();
}
