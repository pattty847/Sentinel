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
    for (int s : {-55,-1,0,5,420}) writer.at(s);
    writer.restart(450); // Two real capture::Writer runs, inside the quarter window.
    for (int s : {455,895,900,905,960,965,1020}) writer.at(s);
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
    size_t twice=0, cuts=0;
    std::map<std::string,std::pair<fs::path,int>> cutSeeds;
    const auto baselineRoot=t.path/"baseline";
    const auto sweep = [&](const fs::path& seed, int startDay, const std::vector<Event>& events,
                           const std::vector<size_t>& stops, bool repeated) {
        for (const auto stop : stops) {
            SCOPED_TRACE(::testing::Message() << (repeated ? "double" : "single")
                << " point=" << stop << " " << events[stop]);
            const auto partial=t.path/"partial", once=t.path/"once";
            if (!seed.empty()) cloneOutput(seed,partial);
            Trace first; first.stop=stop;
            ASSERT_FALSE(run(t.path/"raw",partial,first,startDay));
            ASSERT_TRUE(first.hit);
            ASSERT_EQ(first.events.back(),events[stop]);
            cloneOutput(partial,once);
            Trace recovery;
            ASSERT_NO_THROW(ASSERT_TRUE(run(t.path/"raw",once,recovery,first.day)));
            same(once,baselineRoot);
            ASSERT_FALSE(HasFailure());
            if (repeated) ++twice;
            // Retain each distinct cut plan (day, fenced tail and ordered native
            // cut/sync steps). Suffix bytes are deliberately excluded: a proven
            // cut discards them without decoding. The single-stop pass above
            // still tests every header/payload/CRC suffix independently.
            std::string plan;
            bool cutting=false;
            for (const auto& e : recovery.events) {
                if (e.name=="journal-record") break;
                cutting = cutting || e.name.starts_with("cut-");
                if (cutting) plan+=e.name+"/";
            }
            if (!repeated && cutting) {
                const auto cp=json::parse(contents(partial/"hmc/BTC-USD/roller.json"));
                plan=std::to_string(first.day)+cp.dump()+plan;
                if (!cutSeeds.contains(plan)) {
                    const auto saved=t.path/("cut-seed-"+std::to_string(cutSeeds.size()));
                    cloneOutput(partial,saved);
                    cutSeeds.emplace(plan,std::pair{saved,first.day});
                }
            }
            fs::remove_all(partial); fs::remove_all(once);
        }
    };
    sweep({},0,baseline.events,order,false);
    ASSERT_FALSE(HasFailure());
    // Interrupt once at the first persistence operation; then sweep EVERY point
    // of that normal restart, including all records, writes, syncs and boundaries.
    // Each case is two interruptions, with the second later in execution. A full
    // Cartesian product of stop pairs is intentionally not the contract.
    const auto seed=t.path/"seed";
    Trace initial; initial.stop=0;
    ASSERT_FALSE(run(t.path/"raw",seed,initial));
    cloneOutput(seed,t.path/"trace");
    Trace resumed;
    ASSERT_TRUE(run(t.path/"raw",t.path/"trace",resumed,initial.day));
    std::vector<size_t> secondOrder(resumed.events.size());
    std::iota(secondOrder.begin(),secondOrder.end(),0);
    sweep(seed,initial.day,resumed.events,secondOrder,true);
    ASSERT_FALSE(HasFailure());
    // The forward trace cannot contain destructive recovery cuts. Sweep every
    // step of every distinct cut plan reached by the single-stop enumeration.
    for (const auto& [plan, saved] : cutSeeds) {
        const auto& [root,day]=saved;
        fs::remove_all(t.path/"trace");
        cloneOutput(root,t.path/"trace");
        Trace recovery;
        ASSERT_TRUE(run(t.path/"raw",t.path/"trace",recovery,day));
        std::vector<size_t> steps;
        bool cutting=false;
        for (size_t i=0;i<recovery.events.size();++i) {
            const auto& e=recovery.events[i];
            if (e.name=="journal-record") break;
            cutting=cutting || e.name.starts_with("cut-");
            if (cutting) steps.push_back(i);
        }
        cuts+=steps.size();
        sweep(root,day,recovery.events,steps,true);
        ASSERT_FALSE(HasFailure());
        fs::remove_all(root);
    }
    RecordProperty("single_stop_points",std::to_string(order.size()));
    RecordProperty("double_stop_cases",std::to_string(twice));
    RecordProperty("recovery_cut_plans",std::to_string(cutSeeds.size()));
    RecordProperty("recovery_cut_points",std::to_string(cuts));
    RecordProperty("journal_record_points",std::to_string(std::count_if(baseline.events.begin(),baseline.events.end(),
        [](const auto& e) { return e.name=="journal-record"; })));
}
