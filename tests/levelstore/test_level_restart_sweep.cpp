#include "LevelTestSupport.hpp"
#include "RestartJournal.hpp"
#include "roller/Roller.hpp"
#include "levelstore/LevelLog.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>

using namespace sentinel;
using namespace sentinel::roller;
using namespace levelstore_test;
using nlohmann::json;
namespace {
constexpr int64_t Epoch = RestartJournal::Epoch, Minute = 60'000;
struct Event {
    std::string name;
    int day;
    int64_t second;
    bool operator==(const Event&) const = default;
};
std::ostream& operator<<(std::ostream& out, const Event& e) {
    return out << "day=" << e.day << " second=" << e.second << " " << e.name;
}
struct Trace {
    std::mutex mutex;
    std::vector<Event> events;
    size_t stop = SIZE_MAX;
    bool hit = false;
    int day = 0;
    int64_t second = -61;
    void point(const char* name) {
        std::lock_guard lock(mutex);
        if (hit) return; // Destruction must drain/join without injecting a second failure.
        events.push_back({name,day,second});
        if (events.size()-1 == stop) {
            hit = true;
            throw std::runtime_error("sweep stop");
        }
    }
};
struct Files : LevelLogFiles {
    Trace& trace;
    explicit Files(Trace& t) : trace(t) {}
    size_t write(std::FILE* f, std::span<const uint8_t> bytes) override {
        const auto n = LevelLogFiles::write(f,bytes);
        trace.point("log-write"); // header, payload and CRC separately, including torn frames
        return n;
    }
    void sync(const fs::path& p, bool directory) override {
        LevelLogFiles::sync(p,directory);
        trace.point(directory ? "log-directory-sync" : "log-file-sync");
    }
    void truncate(const fs::path& p, uint64_t n) override {
        LevelLogFiles::truncate(p,n); trace.point("cut-truncate");
    }
    void remove(const fs::path& p) override {
        LevelLogFiles::remove(p); trace.point("cut-remove");
    }
};
std::string contents(const fs::path& p) {
    std::ifstream in(p,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}
std::map<std::string,std::string> tree(const fs::path& root, const std::string& ext) {
    std::map<std::string,std::string> result;
    for (const auto& f : fs::recursive_directory_iterator(root))
        if (f.is_regular_file() && f.path().extension()==ext && !f.path().filename().string().starts_with("._"))
            result[fs::relative(f.path(),root).string()] = contents(f.path());
    return result;
}
void same(const fs::path& actual, const fs::path& expected) {
    EXPECT_EQ(tree(actual,".lcl"),tree(expected,".lcl"));
    EXPECT_EQ(tree(actual,".hmc2"),tree(expected,".hmc2"));
    EXPECT_EQ(tree(actual,".state"),tree(expected,".state"));
    EXPECT_EQ(json::parse(contents(actual/"hmc/BTC-USD/roller.json")),
              json::parse(contents(expected/"hmc/BTC-USD/roller.json")));
}
// Daily calls are the production ShadowRoller contract. Resume the interrupted day,
// never rerun a completed earlier day against a later day's checkpoint.
bool run(const fs::path& raw, const fs::path& root, Trace& trace, int startDay = 0) {
    for (int day=startDay; day<2; ++day) {
        trace.day=day; trace.second=-61;
        recording::BookRecorder* recorder=nullptr;
        LevelRecorder* level=nullptr;
        std::vector<uint8_t> state;
        RollOptions o{raw,root/"hmc","BTC-USD",day ? Epoch : Epoch-Minute,day ? Epoch+16*Minute : Epoch};
        o.levelLogRoot=root/"log";
        o.levelLogFiles=std::make_shared<Files>(trace);
        o.persistencePointForTest=[&](const char* name) { trace.point(name); };
        o.onRecorder=[&](auto* p,auto*,int64_t) { recorder=p; };
        o.onLevelRecorder=[&](auto* p) {
            if (!p && level && recorder && !trace.hit) {
                const auto bytes=recorder->exportState();
                state=level->exportState(recorder->checkpointClock(o.product),bytes);
            }
            level=p;
        };
        o.nextRecord=[&](JournalReader& reader,JournalRecord& record) {
            if (!reader.next(record)) return false;
            trace.second=(record.record.time.systemNs/1'000'000-Epoch)/1000;
            return true;
        };
        // Serialize the worker's real writes with the caller's observation points.
        // This changes scheduling only: all writes/syncs still use native I/O.
        o.onApplied=[&](const auto&) { if (recorder) recorder->drain(); };
        o.afterRecordForTest=[&](auto) { trace.point("journal-record"); };
        o.afterAnchorForTest=[&](auto) { trace.point("anchor-written"); };
        try { roll(o); }
        catch (...) { if (trace.hit) return false; throw; }
        if (trace.hit) return false; // A worker failure can be observed at the next fence.
        if (state.empty()) throw std::runtime_error("missing final C state");
        std::ofstream out(root/(std::to_string(day)+".state"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(state.data()),state.size());
    }
    return true;
}
void journal(const fs::path& root) {
    RestartJournal writer(root);
    for (int s : {-55,-1,0,5,60,120,180,240,300,360,420}) writer.at(s);
    writer.restart(450); // Two real capture::Writer runs, inside the quarter window.
    for (int s : {455,480,540,600,660,720,780,840,895,900,905,960,965,1020}) writer.at(s);
}
void cloneOutput(const fs::path& source, const fs::path& destination) {
    fs::copy(source,destination,fs::copy_options::recursive);
}
}
TEST(LevelRestartSweep, EveryRecordAndPersistencePoint) {
    TempDir t;
    journal(t.path/"raw");
    Trace baseline;
    ASSERT_TRUE(run(t.path/"raw",t.path/"baseline",baseline));
    ASSERT_FALSE(tree(t.path/"baseline",".lcl").empty());
    ASSERT_FALSE(tree(t.path/"baseline",".hmc2").empty());
    std::vector<size_t> order(baseline.events.size());
    std::iota(order.begin(),order.end(),0);
    // Fail early on the exact anchor/checkpoint position, without sampling away
    // any other point. The rest of the trace is still swept in its entirety.
    const auto quarter=std::find_if(order.begin(),order.end(),[&](size_t i) {
        const auto& e=baseline.events[i];
        return e.day==1 && e.second==900 && e.name=="checkpoint-written";
    });
    ASSERT_NE(quarter,order.end());
    std::rotate(order.begin(),quarter,quarter+1);
    size_t twice=0;
    for (const auto stop : order) {
        SCOPED_TRACE(::testing::Message() << "point=" << stop << " " << baseline.events[stop]);
        const auto partial=t.path/"partial", once=t.path/"once", twiceRoot=t.path/"twice";
        Trace first; first.stop=stop;
        ASSERT_FALSE(run(t.path/"raw",partial,first));
        ASSERT_TRUE(first.hit);
        ASSERT_EQ(first.events.back(),baseline.events[stop]);
        cloneOutput(partial,once);
        Trace recovery;
        ASSERT_NO_THROW(ASSERT_TRUE(run(t.path/"raw",once,recovery,first.day)));
        same(once,t.path/"baseline");
        ASSERT_FALSE(HasFailure());
        // Sweep every cut operation and its surrounding syncs on recovery, plus
        // a later forward point for every first stop (including the final point,
        // whose next operation is a restart sync). This is not a Cartesian sweep
        // of all pairs of forward points; both single and repeated interruptions
        // visit every forward point and every reachable recovery cut step.
        std::vector<size_t> secondStops;
        bool forward=false;
        for (size_t i=0;i<recovery.events.size();++i) {
            const auto& e=recovery.events[i];
            if (!forward && (e.name.starts_with("cut-") || e.name.starts_with("log-") && e.name.ends_with("sync")))
                secondStops.push_back(i);
            if (e.name=="journal-record") forward=true;
        }
        const auto later=std::find_if(recovery.events.begin(),recovery.events.end(),[&](const auto& e) {
            return e.day>first.day || e.day==first.day && e.second>first.events.back().second;
        });
        if (later!=recovery.events.end()) secondStops.push_back(later-recovery.events.begin());
        else if (!recovery.events.empty()) secondStops.push_back(recovery.events.size()-1);
        std::sort(secondStops.begin(),secondStops.end());
        secondStops.erase(std::unique(secondStops.begin(),secondStops.end()),secondStops.end());
        ASSERT_FALSE(secondStops.empty());
        for (const auto second : secondStops) {
            SCOPED_TRACE(::testing::Message() << "second=" << second << " " << recovery.events[second]);
            cloneOutput(partial,twiceRoot);
            Trace again; again.stop=second;
            ASSERT_FALSE(run(t.path/"raw",twiceRoot,again,first.day));
            ASSERT_TRUE(again.hit);
            ASSERT_EQ(again.events.back(),recovery.events[second]);
            Trace finish;
            ASSERT_NO_THROW(ASSERT_TRUE(run(t.path/"raw",twiceRoot,finish,again.day)));
            same(twiceRoot,t.path/"baseline");
            ASSERT_FALSE(HasFailure());
            ++twice;
            fs::remove_all(twiceRoot);
        }
        fs::remove_all(partial); fs::remove_all(once);
    }
    RecordProperty("single_stop_points",std::to_string(order.size()));
    RecordProperty("double_stop_cases",std::to_string(twice));
    RecordProperty("journal_record_points",std::to_string(std::count_if(baseline.events.begin(),baseline.events.end(),
        [](const auto& e) { return e.name=="journal-record"; })));
}
