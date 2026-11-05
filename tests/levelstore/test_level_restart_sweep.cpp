#include "LevelTestSupport.hpp"
#include "RestartJournal.hpp"
#include "roller/Roller.hpp"
#include "levelstore/LevelLog.hpp"
#include "levelstore/SealStore.hpp"
#include <gtest/gtest.h>
#include <exception>
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
// Full checkpoint equality requires finishing beyond the next quarter after
// floor fallback: inside that window the resumed checkpoint has anchor == null.
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
            if (!p && level && recorder && !trace.hit && std::uncaught_exceptions()==0) {
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
        o.onApplied=[&](const auto&) {
            if (recorder) recorder->drain();
            if (trace.hit) throw std::runtime_error("sweep worker stop");
        };
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
    for (int s : {455,840,895,900,905,960,965}) writer.at(s);
}
// Forward traces have no replay-validation fence. Include the native recovery
// prefix and the sync immediately preceding that fence, even after overlap records.
std::vector<size_t> recoveryPoints(const std::vector<Event>& events) {
    std::vector<size_t> points;
    bool prefix=true;
    for (size_t i=0;i<events.size();++i) {
        const auto& name=events[i].name;
        if (name=="journal-record") prefix=false;
        const auto sync = [](const std::string& n) { return n=="log-file-sync" || n=="log-directory-sync"; };
        if (prefix && (name.starts_with("cut-") || sync(name))) points.push_back(i);
        if (name=="recovery-replayed") {
            for (size_t j=i; j>0 && sync(events[j-1].name); --j) points.push_back(j-1);
            points.push_back(i);
        }
    }
    std::sort(points.begin(),points.end());
    points.erase(std::unique(points.begin(),points.end()),points.end());
    return points;
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
    size_t twice=0, recoveryStops=0, cuts=0, validationFences=0;
    std::map<std::string,std::pair<fs::path,int>> recoverySeeds;
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
            if (events[stop]==Event{"checkpoint-written",1,900}) {
                const auto cp=json::parse(contents(partial/"hmc/BTC-USD/roller.json"));
                const auto& day=cp.at("days").at(std::to_string(Epoch));
                ASSERT_EQ(day.at("pos"),day.at("level").at("anchor").at("pos"));
                ASSERT_EQ(day.at("level").at("anchor").at("boundary"),Epoch+15*Minute);
            }
            cloneOutput(partial,once);
            Trace recovery;
            ASSERT_NO_THROW(ASSERT_TRUE(run(t.path/"raw",once,recovery,first.day)));
            same(once,baselineRoot);
            ASSERT_FALSE(HasFailure());
            if (repeated) ++twice;
            // Each checkpoint and ordered recovery plan is a distinct seed.
            // Suffix bytes are excluded: cuts discard them without decoding;
            // each torn write still has its own single-stop case above.
            const auto steps=recoveryPoints(recovery.events);
            const bool hasRecovery=std::any_of(recovery.events.begin(),recovery.events.end(),[](const auto& e) {
                return e.name.starts_with("cut-") || e.name=="recovery-replayed";
            });
            if (!repeated && hasRecovery) {
                const auto cp=json::parse(contents(partial/"hmc/BTC-USD/roller.json"));
                auto plan=std::to_string(first.day)+cp.dump();
                for (auto i : steps) plan+=recovery.events[i].name+"/";
                if (!recoverySeeds.contains(plan)) {
                    const auto saved=t.path/("recovery-seed-"+std::to_string(recoverySeeds.size()));
                    cloneOutput(partial,saved);
                    recoverySeeds.emplace(plan,std::pair{saved,first.day});
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
    // Forward traces cannot contain recovery cuts or replay-validation fences.
    // Sweep every step of every distinct plan reached by the first enumeration.
    for (const auto& [plan, saved] : recoverySeeds) {
        const auto& [root,day]=saved;
        fs::remove_all(t.path/"trace");
        cloneOutput(root,t.path/"trace");
        Trace recovery;
        ASSERT_TRUE(run(t.path/"raw",t.path/"trace",recovery,day));
        const auto steps=recoveryPoints(recovery.events);
        recoveryStops+=steps.size();
        for (const auto i : steps) {
            cuts+=recovery.events[i].name.starts_with("cut-");
            validationFences+=recovery.events[i].name=="recovery-replayed";
        }
        sweep(root,day,recovery.events,steps,true);
        ASSERT_FALSE(HasFailure());
        fs::remove_all(root);
    }
    json counts=json::object(), trace=json::array();
    for (const auto& e : baseline.events) {
        counts[e.name]=counts.value(e.name,0)+1;
        trace.push_back({{"day",e.day},{"second",e.second},{"point",e.name}});
    }
    for (const auto* name : {"log-write","log-file-sync","log-directory-sync",
             "checkpoint-mid-write","checkpoint-temp-written","checkpoint-temp-synced",
             "checkpoint-renamed","checkpoint-file-synced","checkpoint-directory-synced",
             "checkpoint-written","anchor-mid-write","anchor-temp-written","anchor-temp-synced",
             "anchor-renamed","anchor-file-synced","anchor-directory-synced","anchor-written"})
        ASSERT_GT(counts.value(name,0),0) << name;
    ASSERT_GT(cuts,0u);
    ASSERT_GT(validationFences,0u);
    ASSERT_EQ(resumed.events.size(),baseline.events.size());
    ASSERT_EQ(counts.at("journal-record"),21);
    RecordProperty("forward_point_counts",counts.dump());
    RecordProperty("forward_trace",trace.dump());
    RecordProperty("single_stop_points",std::to_string(order.size()));
    RecordProperty("double_stop_cases",std::to_string(twice));
    RecordProperty("recovery_plans",std::to_string(recoverySeeds.size()));
    RecordProperty("recovery_points",std::to_string(recoveryStops));
    RecordProperty("cut_operations",std::to_string(cuts));
    RecordProperty("replay_validation_fences",std::to_string(validationFences));
    RecordProperty("journal_record_points",std::to_string(std::count_if(baseline.events.begin(),baseline.events.end(),
        [](const auto& e) { return e.name=="journal-record"; })));
}

TEST(LevelRestartSweep, EveryHourSealPointRestartsWithPairedAnchorIdentity) {
    TempDir t;
    RestartJournal writer(t.path / "raw");
    for (int s = 0; s <= 45 * 60; s += 5)
        writer.at(s);
    std::vector<std::string> points;
    const auto runSeal = [&](const fs::path &root, size_t stop) {
        size_t index = 0;
        bool hit = false;
        recording::BookRecorder *recorder = nullptr;
        RollOptions o{t.path / "raw", root / "hmc", "BTC-USD", Epoch, Epoch + 75 * Minute};
        o.levelLogRoot = root / "log";
        o.onRecorder = [&](auto *r, auto *, int64_t) { recorder = r; };
        o.onApplied = [&](const auto &) {
            recorder->drain();
            if (hit)
                throw std::runtime_error("seal worker stop");
        };
        o.persistencePointForTest = [&](const char *name) {
            if (!std::string_view(name).starts_with("seal-") || hit)
                return;
            if (root.filename() == "baseline-seal")
                points.push_back(name);
            if (index++ == stop) {
                hit = true;
                throw std::runtime_error("seal stop");
            }
        };
        try {
            roll(o);
        } catch (...) {
            if (!hit)
                throw;
        }
        return hit;
    };
    ASSERT_FALSE(runSeal(t.path / "growing-seal", SIZE_MAX));
    for (int s = 45 * 60 + 5; s <= 76 * 60; s += 5)
        writer.at(s);
    ASSERT_FALSE(runSeal(t.path / "baseline-seal", SIZE_MAX));
    ASSERT_FALSE(points.empty());
    const auto everyFile = [&](const fs::path &root) {
        std::map<std::string, std::string> files;
        for (const auto &e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file())
                files[fs::relative(e.path(), root).string()] = contents(e.path());
        return files;
    };
    const auto wanted = everyFile(t.path / "baseline-seal");
    for (size_t i = 0; i < points.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << i << " " << points[i]);
        const auto out = t.path / ("seal-stop-" + std::to_string(i));
        ASSERT_TRUE(runSeal(out, i));
        ASSERT_FALSE(runSeal(out, SIZE_MAX));
        EXPECT_EQ(everyFile(out), wanted);
        ASSERT_FALSE(HasFailure());
    }
    ASSERT_FALSE(runSeal(t.path / "growing-seal", SIZE_MAX));
    EXPECT_EQ(everyFile(t.path / "growing-seal"), wanted);
    RecordProperty("seal_points", nlohmann::json(points).dump());
}

TEST(LevelRestartSweep, AbandonedRecorderTempsAreRemovedWithoutChangingCommittedFiles) {
    TempDir t;
    journal(t.path / "raw");
    Trace baseline;
    ASSERT_TRUE(run(t.path / "raw", t.path / "expected", baseline));
    const auto everyFile = [&](const fs::path &root) {
        std::map<std::string, std::string> files;
        for (const auto &e : fs::recursive_directory_iterator(root))
            if (fs::is_regular_file(e.symlink_status()))
                files[e.path().lexically_relative(root).string()] = contents(e.path());
        return files;
    };
    const auto add = [](const fs::path &p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "uncommitted temporary bytes";
    };
    const std::vector<fs::path> unrelated = {
        "hmc/BTC-USD/roller.json.backup-file", "hmc/BTC-USD/roller.json.a!2345",
        "hmc/BTC-USD/anchors/2026-10-05/0001.anchor.abc123",
        "hmc/BTC-USD/anchors/2026-02-30/0000.anchor.abc123",
        "hmc/BTC-USD/anchors/notes/0000.anchor.abc123"};
    for (const auto &p : unrelated)
        add(t.path / "expected" / p);
    const auto wanted = everyFile(t.path / "expected");
    for (bool interrupt : {false, true}) {
        const auto output = t.path / (interrupt ? "interrupted" : "resumed");
        cloneOutput(t.path / "expected", output);
        add(output / "hmc/BTC-USD/roller.json.LtOKuo");
        add(output / "hmc/BTC-USD/anchors/2026-10-05/0015.anchor.Ab123z");
        const auto link = output / "hmc/BTC-USD/roller.json.Sym123";
        fs::create_symlink("roller.json", link);
        Trace recovery;
        if (interrupt) {
            // The first cleanup is followed by a durable directory sync. Simulate
            // another crash there; retry must remove the remaining temp safely.
            // Locate the cleanup seam from a normal restart, then inject there.
            const auto probe = t.path / "probe";
            cloneOutput(output, probe);
            Trace trace;
            ASSERT_TRUE(run(t.path / "raw", probe, trace, 1));
            const auto found = std::find_if(trace.events.begin(), trace.events.end(), [](const auto &e) {
                return e.name == "c-recovery-temp-removed";
            });
            ASSERT_NE(found, trace.events.end());
            recovery.stop = size_t(found - trace.events.begin());
            ASSERT_FALSE(run(t.path / "raw", output, recovery, 1));
        }
        Trace retry;
        ASSERT_TRUE(run(t.path / "raw", output, retry, 1));
        EXPECT_EQ(everyFile(output), wanted);
        EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    }
}

TEST(LevelRestartSweep, DayManifestRetentionAndVerifiedMarkerWithMessagesEveryMinute) {
    constexpr int64_t Hour = 60*Minute, Day = 24*Hour;
    TempDir t;
    RestartJournal writer(t.path / "raw");
    using Tree = std::map<std::string, std::string>;
    const auto everyFile = [&](const fs::path &root) {
        Tree result;
        for (const auto &e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file()) result[e.path().lexically_relative(root).string()] = contents(e.path());
        return result;
    };
    struct RunResult { bool hit = false; std::vector<std::string> points; int liveHours = 0; };
    const auto runDay = [&](const fs::path &root, int day, size_t stop = SIZE_MAX) {
        RunResult result;
        recording::BookRecorder *recorder = nullptr;
        LevelRecorder *level = nullptr;
        RollOptions o{t.path / "raw", root / "hmc", "BTC-USD", Epoch+day*Day,
                      Epoch+day*Day+(day ? 75*Minute : Day)};
        o.levelLogRoot = root / "log";
        o.levelLogRetentionMs = Hour;
        o.onRecorder = [&](auto *r, auto *, int64_t) { recorder = r; };
        o.onLevelRecorder = [&](auto *c) { level = c; };
        o.onApplied = [&](const auto &) {
            if (recorder) recorder->drain();
            if (result.hit) throw std::runtime_error("day sweep worker stop");
        };
        o.persistencePointForTest = [&](const char *name) {
            if (!std::string_view(name).starts_with("seal-") || result.hit) return;
            if (std::string_view(name) == "seal-hour-end" && level && level->openBlock().bucketStartMs >= o.fromMs) {
                const auto hourStart = level->openBlock().bucketStartMs / Hour * Hour;
                const auto sealed = SealStore(o.levelLogRoot, o.product).read(hourStart);
                EXPECT_EQ(endImage(sealed.timeline), (BookImage{hourStart+Hour, level->observed(), level->book(),
                                                               level->priceDigits(), level->sizeDigits()}));
                ++result.liveHours;
            }
            // Recovery re-verifies earlier seals too. Sweep the forward publication
            // of the final hour, including all marker/day/fence/unlink seams.
            if (!level || level->openBlock().bucketStartMs < o.fromMs + (day ? 59 : 1439)*Minute) return;
            result.points.emplace_back(name);
            if (result.points.size()-1 == stop) {
                result.hit = true;
                throw std::runtime_error("day persistence stop");
            }
        };
        try { roll(o); } catch (...) { if (!result.hit) throw; }
        return result;
    };
    for (int m = 0; m <= 1425; ++m) writer.at(m*60+2);
    ASSERT_FALSE(runDay(t.path/"seed-day", 0).hit);
    for (int m = 1426; m <= 1485; ++m) writer.at(m*60+2);
    cloneOutput(t.path/"seed-day", t.path/"seed-retention");
    ASSERT_FALSE(runDay(t.path/"seed-retention", 0).hit);
    ASSERT_FALSE(runDay(t.path/"seed-retention", 1).hit);
    for (int m = 1486; m <= 1516; ++m) writer.at(m*60+2);
    auto first = runDay(t.path/"baseline-day", 0);
    auto second = runDay(t.path/"baseline-day", 1);
    ASSERT_EQ(first.liveHours, 24);
    ASSERT_EQ(second.liveHours, 1);
    const auto wanted = everyFile(t.path/"baseline-day");
    int cases = 0;
    for (int day : {0, 1}) {
        const auto seed = t.path/(day ? "seed-retention" : "seed-day");
        const auto probe = t.path/(day ? "probe-retention" : "probe-day");
        cloneOutput(seed, probe);
        const auto trace = runDay(probe, day);
        ASSERT_FALSE(trace.hit);
        ASSERT_FALSE(trace.points.empty());
        if (day) EXPECT_EQ(std::count(trace.points.begin(), trace.points.end(), "seal-log-pruned"), 24);
        else EXPECT_EQ(std::count(trace.points.begin(), trace.points.end(), "seal-day-manifest-written"), 1);
        EXPECT_EQ(std::count(trace.points.begin(), trace.points.end(), "seal-marked-verified"), 1);
        for (size_t stop = 0; stop < trace.points.size(); ++stop) {
            SCOPED_TRACE(::testing::Message() << "day=" << day << " stop=" << stop << " " << trace.points[stop]);
            const auto out = t.path/"attempt-day";
            cloneOutput(seed, out);
            auto interrupted = runDay(out, day, stop);
            ASSERT_TRUE(interrupted.hit);
            ASSERT_EQ(interrupted.points.back(), trace.points[stop]);
            ASSERT_FALSE(runDay(out, day).hit);
            if (!day) ASSERT_FALSE(runDay(out, 1).hit);
            EXPECT_EQ(everyFile(out), wanted);
            ASSERT_FALSE(HasFailure());
            fs::remove_all(out);
            ++cases;
        }
        RecordProperty(day ? "retention_points" : "day_manifest_points", json(trace.points).dump());
    }
    RecordProperty("day_retention_cases", cases);
    RecordProperty("live_hour_images_compared", first.liveHours+second.liveHours);
    RecordProperty("journal_minutes_with_real_updates", 1517);
}
