#include "LevelTestSupport.hpp"
#include "RestartJournal.hpp"
#include "Crc32.hpp"
#include "roller/CheckpointBytes.hpp"
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
#endif
using namespace sentinel;
using namespace sentinel::roller;
using namespace levelstore_test;
using nlohmann::json;
namespace {
constexpr auto Epoch = RestartJournal::Epoch;
constexpr int64_t Minute = 60'000;
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
    EXPECT_EQ(state(a),state(b));
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
    for (int m : {20,40,60,90}) { journal.grow(m); ASSERT_NO_THROW(roll(stepped)); }
    ASSERT_NO_THROW(roll(options(t.path/"raw",t.path/"one")));
    equal(t.path/"one",t.path/"stepped");
    auto baseline=options(t.path/"raw",t.path/"baseline"); baseline.levelLogRoot.clear();
    ASSERT_NO_THROW(roll(baseline));
    EXPECT_EQ(tree(baseline.outputRoot,".hmc2"),tree(t.path/"one/hmc",".hmc2"));
}
TEST(LevelRestart, RetryAfterFailedCUsesProcessRestartPath) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    ASSERT_NO_THROW(roll(options(t.path/"raw",t.path/"one")));
    auto o=options(t.path/"raw",t.path/"retry");
    auto fault=std::make_shared<FaultFiles>(); o.levelLogFiles=fault;
    o.onApplied=[&](const JournalRecord& r) { if (r.record.time.systemNs/1'000'000>=Epoch+35*Minute) fault->armed=true; };
    EXPECT_THROW(roll(o),std::exception);
    o.onApplied={}; o.levelLogFiles={};
    ASSERT_NO_THROW(roll(o)); equal(t.path/"one",t.path/"retry");
}
TEST(LevelRestart, HmcOnlyResumeRefusesBeforeOutput) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); o.levelLogRoot.clear(); roll(o);
    o.levelLogRoot=t.path/"log";
    EXPECT_THROW(roll(o),std::exception); EXPECT_FALSE(fs::exists(o.levelLogRoot));
    fs::remove(o.outputRoot/"BTC-USD/roller.json");
    // Keep anchors but avoid the independent HMC2 provenance check.
    o.anchorRoot=o.outputRoot; o.outputRoot=t.path/"fresh";
    EXPECT_THROW(roll(o),std::exception); EXPECT_FALSE(fs::exists(o.levelLogRoot));
}
TEST(LevelRestart, PairedCheckpointMismatchRefusesBeforeCut) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); roll(o);
    const auto path=o.outputRoot/"BTC-USD/roller.json";
    const auto original=contents(path); const auto before=tree(o.levelLogRoot,".lcl");
    for (bool length : {false,true}) {
        auto cp=json::parse(original); auto& bytes=cp["days"][std::to_string(Epoch)]["recorder"];
        auto payload=unpackCheckpointBytes(bytes.get<std::string>()); payload.resize(payload.size()-4);
        auto decoded=json::from_cbor(payload);
        decoded["stats"][1]=length ? 65536 : 1;
        payload=json::to_cbor(decoded);
        const auto crc=sentinel::crc32(payload.data(),payload.size());
        for (int i=0;i<4;++i) payload.push_back(uint8_t(crc>>(8*i)));
        bytes=packCheckpointBytes(payload);
        std::ofstream(path)<<cp.dump();
        try { roll(o); FAIL()<<"accepted mismatched recorder pair"; }
        catch (const std::exception& e) { EXPECT_NE(std::string(e.what()).find("not paired"),std::string::npos)<<e.what(); }
        EXPECT_EQ(tree(o.levelLogRoot,".lcl"),before);
    }
}
#ifndef _WIN32
TEST(LevelRestart, KillAndRestartMatchesAllBytes) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    ASSERT_NO_THROW(roll(options(t.path/"raw",t.path/"one")));
    for (const std::string point : {"initial-append","append","checkpoint-synced","checkpoint-mid-write","anchor-mid-write","cut"}) {
        SCOPED_TRACE(point); const auto root=t.path/point;
        const auto child=::fork(); ASSERT_GE(child,0);
        if (!child) {
            try {
                auto o=options(t.path/"raw",root); auto f=std::make_shared<FaultFiles>(); o.levelLogFiles=f;
                bool ready=false; f->killWrite=true;
                o.onApplied=[&](const JournalRecord& r) {
                    ready=r.record.time.systemNs/1'000'000>=Epoch+(point=="initial-append" ? 0 : 35*Minute);
                    if (ready && (point=="initial-append" || point=="append" || point=="cut")) f->armed=true;
                };
                o.persistencePointForTest=[&](const char* p) { if (ready && point==p) ::kill(::getpid(),SIGKILL); };
                roll(o); ::_exit(3);
            } catch (...) { ::_exit(4); }
        }
        int status=0; ASSERT_EQ(::waitpid(child,&status,0),child);
        ASSERT_TRUE(WIFSIGNALED(status)); ASSERT_EQ(WTERMSIG(status),SIGKILL);
        if (point=="cut") {
            const auto cutting=::fork(); ASSERT_GE(cutting,0);
            if (!cutting) {
                try { auto o=options(t.path/"raw",root); auto f=std::make_shared<FaultFiles>();
                    f->killCut=true; o.levelLogFiles=f; roll(o); ::_exit(3); } catch (...) { ::_exit(4); }
            }
            ASSERT_EQ(::waitpid(cutting,&status,0),cutting);
            ASSERT_TRUE(WIFSIGNALED(status)); ASSERT_EQ(WTERMSIG(status),SIGKILL);
        }
        ASSERT_NO_THROW(roll(options(t.path/"raw",root))); equal(t.path/"one",root);
    }
}
#endif

TEST(LevelRestart, GrowingRangeAndBeforeFirstQuarterMatchOneRoll) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"stepped"); o.allowGrowingEnd=true;
    for (int m : {15,30,60,90}) { o.toMs=Epoch+m*Minute; ASSERT_NO_THROW(roll(o)); }
    auto one=options(t.path/"raw",t.path/"one"); one.allowGrowingEnd=true; roll(one);
    equal(t.path/"one",t.path/"stepped");
    auto shortRoll=options(t.path/"raw",t.path/"short"); shortRoll.fromMs=Epoch+7*Minute; shortRoll.toMs=Epoch+9*Minute;
    roll(shortRoll); const auto before=tree(shortRoll.levelLogRoot,".lcl");
    ASSERT_NO_THROW(roll(shortRoll)); EXPECT_EQ(tree(shortRoll.levelLogRoot,".lcl"),before);
}
TEST(LevelRestart, PairedAnchorMismatchRefusesBeforeCut) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"attempt"); roll(o);
    const auto candidates=AnchorStore::candidates(o.outputRoot,"BTC-USD",Epoch);
    ASSERT_FALSE(candidates.empty());
    auto a=AnchorStore::load(candidates.front());
    auto j=json::from_cbor(a.recorder.bytes.begin(),a.recorder.bytes.end()-4);
    j["stats"][1]=1; a.recorder.bytes=json::to_cbor(j);
    const auto crc=sentinel::crc32(a.recorder.bytes.data(),a.recorder.bytes.size());
    for (int i=0;i<4;++i) a.recorder.bytes.push_back(uint8_t(crc>>(8*i)));
    AnchorStore::write(o.outputRoot,a);
    const auto before=tree(o.levelLogRoot,".lcl");
    EXPECT_THROW(roll(o),std::exception); EXPECT_EQ(tree(o.levelLogRoot,".lcl"),before);
}

TEST(LevelRestart, LegacyAnchorEnvelopeRemainsReadableWithoutC) {
    TempDir t; RestartJournal journal(t.path/"raw"); journal.grow(90);
    auto o=options(t.path/"raw",t.path/"old"); o.levelLogRoot.clear(); roll(o);
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
    ASSERT_NO_THROW(roll(o));
}

TEST(LevelRestart, EmptyAndImageFencesDiscardUnverifiedSuffixBeforeOpen) {
    for (bool warmup : {false,true}) {
        SCOPED_TRACE(warmup); TempDir t; RestartJournal journal(t.path/"raw",warmup); journal.grow(90);
        roll(options(t.path/"raw",t.path/"one"));
        auto o=options(t.path/"raw",t.path/"restart");
        o.afterAnchorForTest=[](int64_t boundary) { if (boundary==Epoch) throw std::runtime_error("stop at first anchor"); };
        ASSERT_THROW(roll(o),std::exception);
        const auto file=LevelLogReader::hourPath(o.levelLogRoot,"BTC-USD",Epoch);
        fs::create_directories(file.parent_path());
        std::ofstream broken(file,std::ios::binary|std::ios::app); broken<<std::string(256,'\0'); broken.close();
        o.afterAnchorForTest={}; ASSERT_NO_THROW(roll(o)); equal(t.path/"one",t.path/"restart");
    }
}
