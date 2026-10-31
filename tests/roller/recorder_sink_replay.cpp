// Offline evidence for the BookRecorder sink: one production roll() of one UTC
// day with sink null, a no-op sink, or the reference consumer. Read-only on the
// journal; all output under OUT_ROOT. Never part of ctest.
#include "../servermodel/RecorderSinkReference.hpp"
#include "IndependentBook.hpp"
#include "roller/Roller.hpp"
#include <QCoreApplication>
#include <mach/mach.h>
#include <sys/resource.h>
#include <zstd.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace recording;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kDay = 86'400'000;

struct NoopSink final : RecorderSink {
    void onReset(const std::string &, int64_t) noexcept override {}
    void onImprecise(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onLevel(const std::string &, int64_t, const LevelChange &) noexcept override {}
    void onApplied(const std::string &, int64_t, bool, bool) noexcept override {}
    void onInvalid(const std::string &, int64_t, const std::string &, bool) noexcept override {}
    void onMinute(const std::string &, int64_t, uint32_t, uint32_t) noexcept override {}
};

// CPU time (ns) of the named thread in this process, or -1.
int64_t threadCpuNs(const char *name) {
    thread_act_array_t threads;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return -1;
    int64_t out = -1;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_extended_info_data_t info;
        mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count) ==
                KERN_SUCCESS && std::strcmp(info.pth_name, name) == 0)
            out = int64_t(info.pth_user_time + info.pth_system_time);
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), n * sizeof(thread_act_t));
    return out;
}
double processCpuMs() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1e3 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e3;
}
size_t zstdSize(const std::vector<uint8_t> &raw) {
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    const auto n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
    return ZSTD_isError(n) ? 0 : n;
}

std::map<int64_t, double> sideOf(const json &arr) {
    std::map<int64_t, double> out;
    for (const auto &e : arr) out.emplace(e.at(0).get<int64_t>(), e.at(1).get<double>());
    return out;
}
// Per-side difference between a book and a snapshot (sizes exact).
json diffSide(const std::map<int64_t, double> &book, const std::map<int64_t, double> &snap) {
    uint64_t onlyBook = 0, onlySnap = 0, sizeMismatch = 0;
    long double absDiff = 0, signedDiff = 0;
    for (const auto &[u, q] : book) {
        const auto it = snap.find(u);
        if (it == snap.end()) { ++onlyBook; absDiff += q; signedDiff += q; }
        else if (it->second != q) { ++sizeMismatch; absDiff += std::abs(q - it->second); signedDiff += q - it->second; }
    }
    for (const auto &[u, q] : snap)
        if (!book.contains(u)) { ++onlySnap; absDiff += q; signedDiff -= q; }
    return {{"onlyBook", onlyBook}, {"onlySnapshot", onlySnap}, {"sizeMismatch", sizeMismatch},
            {"absSizeDiff", double(absDiff)}, {"bookMinusSnapshotSize", double(signedDiff)}};
}
bool differs(const json &d) {
    return d.at("onlyBook") != 0 || d.at("onlySnapshot") != 0 || d.at("sizeMismatch") != 0;
}

// Ground-truth checks: snapshot agreement (check 6) and the independent rebuild (check 7).
struct Truth {
    std::string product;
    e1a::IndependentBook indep;
    uint64_t feedInvalids = 0, lastFeedInvalids = 0, updateMessages = 0, droppedRecorderUpdates = 0,
             droppedRecorderLevels = 0;
    uint64_t gaps = 0, gapsInvalidated = 0, gapsUnaccounted = 0;
    json snapshots = json::array();
    std::map<std::string, uint64_t> classes;
    uint64_t continuousFindings = 0, sinkRecorderDisagreements = 0;
    uint64_t anchors = 0, anchorMismatches = 0, anchorLevels = 0, validityMismatches = 0;
    json anchorExamples = json::array();
    Truth(std::string p, uint64_t dropIndependent) : product(p), indep(p) { indep.dropUpdateEvent = dropIndependent; }

    void record(const sentinel::roller::JournalRecord &in) {
        const bool gap = indep.apply(in);
        const bool invalidated = feedInvalids != lastFeedInvalids;
        lastFeedInvalids = feedInvalids;
        if (!gap) return;
        ++gaps;
        (invalidated ? gapsInvalidated : gapsUnaccounted)++;
    }
    // Producer thread, before the recorder takes the snapshot. exportState drains,
    // so the recorder and the sink consumer reflect every earlier journal record.
    void snapshot(recording::BookRecorder *recorder, const sinkref::ReferenceConsumer &consumer, int64_t at,
                  int64_t day, const std::vector<Level> &levels) {
        json row = {{"envelopeMs", at}, {"inDay", at >= day && at < day + kDay}, {"levels", levels.size()}};
        std::string cls = "start";
        json state;
        if (recorder) {
            const auto bytes = recorder->exportState();
            if (bytes.size() < 5) throw std::runtime_error("degraded recorder state at a snapshot");
            state = json::from_cbor(bytes.begin(), bytes.end() - 4);
            if (state.at("symbols").contains(product) && state["symbols"][product].at("initialized").get<bool>())
                cls = state["symbols"][product].at("valid").get<bool>() ? "continuous" : "afterInvalidation";
        }
        row["class"] = cls;
        ++classes[cls];
        if (cls != "start") {
            const double scale = state.at("policy").at("priceScale").get<double>();
            std::map<int64_t, double> snap[2];
            for (const auto &l : levels) snap[l.isBid ? 0 : 1][std::llround(l.price * scale)] = l.size;
            const auto &s = state["symbols"][product];
            bool any = false, sinkSame = true;
            for (int side = 0; side < 2; ++side) {
                const auto book = sideOf(s.at(side ? "asks" : "bids"));
                std::map<int64_t, double> sink;
                for (const auto &[u, q] : consumer.live(product, side == 0)) sink.emplace(u, q);
                const auto d = diffSide(book, snap[side]);
                row[side ? "askRecorder" : "bidRecorder"] = d;
                row[side ? "askSink" : "bidSink"] = diffSide(sink, snap[side]);
                any = any || differs(d);
                sinkSame = sinkSame && sink == book;
            }
            row["sinkEqualsRecorder"] = sinkSame;
            if (!sinkSame) ++sinkRecorderDisagreements;
            if (cls == "continuous" && any) ++continuousFindings;
        }
        snapshots.push_back(std::move(row));
    }
    // Quarter anchors: exact decimal level sets and sizes, recorder vs independent rebuild.
    void compareAnchor(const std::vector<uint8_t> &bytes) {
        ++anchors;
        if (bytes.size() < 5) throw std::runtime_error("degraded recorder state at an anchor");
        const auto state = json::from_cbor(bytes.begin(), bytes.end() - 4);
        const double scale = state.at("policy").at("priceScale").get<double>();
        int decimals = 0;
        while (decimals < 18 && std::pow(10.0, decimals) < scale) ++decimals;
        if (std::pow(10.0, decimals) != scale) throw std::runtime_error("priceScale is not a power of ten");
        const bool has = state.at("symbols").contains(product);
        const bool valid = has && state["symbols"][product].at("valid").get<bool>();
        const auto note = [&](const std::string &what) {
            if (anchorExamples.size() < 8) anchorExamples.push_back("anchor " + std::to_string(anchors) + ": " + what);
        };
        if (valid != indep.valid) {
            ++validityMismatches; ++anchorMismatches;
            note(std::string("validity recorder=") + (valid ? "1" : "0") + " independent=" + (indep.valid ? "1" : "0"));
            return;
        }
        if (!valid) return;
        bool mismatch = false;
        for (int side = 0; side < 2; ++side) {
            std::map<std::string, std::string> rec;
            for (const auto &e : state["symbols"][product].at(side ? "asks" : "bids"))
                rec.emplace(e1a::unitsToDecimal(e.at(0).get<int64_t>(), decimals), e1a::doubleToDecimal(e.at(1).get<double>()));
            anchorLevels += rec.size();
            for (const auto &[p, q] : rec) {
                const auto it = indep.side[side].find(p);
                if (it == indep.side[side].end()) { mismatch = true; note(std::string(side ? "ask " : "bid ") + p + " only in recorder"); }
                else if (it->second != q) { mismatch = true; note(std::string(side ? "ask " : "bid ") + p + " size recorder=" + q + " independent=" + it->second); }
            }
            for (const auto &[p, q] : indep.side[side])
                if (!rec.contains(p)) { mismatch = true; note(std::string(side ? "ask " : "bid ") + p + " only in independent"); }
        }
        if (mismatch) ++anchorMismatches;
    }
    bool clean() const {
        return !continuousFindings && !gapsUnaccounted && !anchorMismatches && !sinkRecorderDisagreements;
    }
    json report() const {
        const auto &c = indep.counts;
        return {{"snapshotClasses", classes}, {"continuousFindings", continuousFindings},
                {"sinkRecorderDisagreements", sinkRecorderDisagreements}, {"snapshots", snapshots},
                {"sequence", {{"discontinuities", gaps}, {"followedByInvalidation", gapsInvalidated},
                              {"unaccounted", gapsUnaccounted}, {"v1Gaps", c.v1SequenceGaps},
                              {"regressions", c.sequenceRegressions}, {"routingReceiptGaps", c.routingGaps},
                              {"journalFramingGaps", c.journalGaps}}},
                {"feedInvalidations", feedInvalids},
                {"independent", {{"records", c.records}, {"frames", c.frames}, {"snapshots", c.snapshots},
                                 {"updateEvents", c.updateEvents}, {"levels", c.levels},
                                 {"connectionBoundaries", c.connectionBoundaries}, {"transport", c.transport},
                                 {"captureInvalidations", c.captureInvalidations}, {"malformed", c.malformed},
                                 {"droppedUpdates", c.droppedUpdates}, {"v1Frames", c.v1Frames},
                                 {"v2Frames", c.v2Frames}, {"frameReferences", c.frameReferences},
                                 {"sequenceJumpsAnyVersion", c.sequenceJumpsAnyVersion}, {"jumps", indep.jumps}}},
                {"droppedRecorderUpdates", droppedRecorderUpdates}, {"droppedRecorderLevels", droppedRecorderLevels}, {"independentSnapshotDiagnostics", indep.diagnostics},
                {"anchorRebuild", {{"anchors", anchors}, {"mismatches", anchorMismatches},
                                   {"validityMismatches", validityMismatches}, {"levelsCompared", anchorLevels},
                                   {"examples", anchorExamples}}}};
    }
};
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 7 || argc % 2 == 0) {
        std::cerr << "usage: recorder_sink_replay RAW_ROOT OUT_ROOT PRODUCT YYYY-MM-DD null|noop|consumer|truth REPORT.json"
                     " [--drop-recorder N] [--drop-independent N] [--drop-recorder-at RECEIVE_MS]"
                     " [--drop-independent-at RECEIVE_MS]\n";
        return 2;
    }
    try {
        const fs::path raw = fs::canonical(argv[1]), out = argv[2];
        const std::string product = argv[3], mode = argv[5];
        if (mode != "null" && mode != "noop" && mode != "consumer" && mode != "truth") throw std::runtime_error("bad mode");
        // truth: the consumer checks plus exchange-snapshot agreement and the independent rebuild.
        const bool truth = mode == "truth", consumerMode = mode == "consumer" || truth;
        uint64_t dropRecorder = 0, dropIndependent = 0; // mutation seams, truth mode only
        int64_t dropRecorderAt = 0, dropIndependentAt = 0; // receive ms
        for (int i = 7; i + 1 < argc; i += 2) {
            const std::string flag = argv[i];
            if (flag == "--drop-recorder") dropRecorder = std::stoull(argv[i + 1]);
            else if (flag == "--drop-independent") dropIndependent = std::stoull(argv[i + 1]);
            else if (flag == "--drop-recorder-at") dropRecorderAt = std::stoll(argv[i + 1]);
            else if (flag == "--drop-independent-at") dropIndependentAt = std::stoll(argv[i + 1]);
            else throw std::runtime_error("unknown option " + flag);
        }
        if ((dropRecorder || dropIndependent || dropRecorderAt || dropIndependentAt) && !truth) throw std::runtime_error("mutations need truth mode");
        if (fs::exists(out) && !fs::is_empty(out)) throw std::runtime_error("OUT_ROOT must be new or empty");
        fs::create_directories(out / "hmc2");
        const auto source = raw.string() + "/", dest = fs::canonical(out).string() + "/";
        if (dest.starts_with(source) || source.starts_with(dest)) throw std::runtime_error("roots overlap");
        const int64_t day = sentinel::roller::parseTime(argv[4]);

        NoopSink noop;
        sinkref::ReferenceConsumer consumer;
        consumer.keepRuns = consumerMode; // cleared at every anchor; tail survey only
        consumer.storeFromMs = day; // the commit floor writes no warmup minute before the day
        sinkref::RowCheck rows;
        std::map<int64_t, int> layersChecked;
        json anchors = json::array();
        uint64_t bookChecks = 0, bookMismatches = 0, levelsCompared = 0;
        std::string firstBookError;
        recording::BookRecorder *recorder = nullptr;
        json recorderStats;
        int64_t workerNs = -1;

        sentinel::roller::RollOptions o;
        o.journalRoot = raw;
        o.outputRoot = out / "hmc2";
        o.product = product;
        o.fromMs = day;
        o.toMs = day + kDay;
        o.useAnchors = false; // fresh root; anchors are still written (quarter-boundary survey)
        o.writeAnchors = true;
        if (mode == "noop") o.recorderSink = &noop;
        Truth t(product, dropIndependent);
        t.indep.dropAtReceiveMs = dropIndependentAt;
        if (consumerMode) {
            o.recorderSink = &consumer;
            // Worker thread, after the minute's HMC2 append: verify, then forget the minute.
            o.publisher = [&](std::shared_ptr<const Hmc2Record> r) {
                if (r->header.tfMs != 60'000) return;
                const auto it = consumer.minutes.find({product, r->bucketStartMs});
                if (it == consumer.minutes.end()) {
                    ++rows.unmatchedRecords;
                    return;
                }
                rows.check(*r, it->second);
                if (++layersChecked[r->bucketStartMs] == 2) {
                    consumer.minutes.erase(it);
                    layersChecked.erase(r->bucketStartMs);
                }
            };
            o.anchorExportForTest = [&](recording::BookRecorder &r) {
                auto state = r.exportState(); // drains: the worker is idle while the consumer is read
                ++bookChecks;
                size_t levels = 0;
                if (const auto e = sinkref::compareBook(state, product, consumer, &levels); !e.empty()) {
                    ++bookMismatches;
                    if (firstBookError.empty()) firstBookError = e;
                }
                levelsCompared += levels;
                // Estimated C additions for a resumable checkpoint (document only). Integrals
                // are normalised to the checkpoint clock, so no lazy `since` is stored.
                // Inherited from exportState: book (units, size), clock, minute, offset,
                // observed, flags, validity, row peaks. Added: per live level run start and
                // open-minute integral; removed levels that still hold an integral; runs
                // closed since the committed watermark; finished-but-unverified minutes;
                // the covered-time cursor.
                const int64_t committed = r.watermarks(product, "near").minuteThroughMs;
                const auto bit = consumer.books.find(product);
                json add = {{"live", json::array()}, {"removed", json::array()}, {"tail", json::array()},
                            {"pending", json::array()}, {"cursor", json::array()}};
                uint64_t live = 0, removed = 0, tail = 0, pendingLevels = 0;
                int64_t clock = 0;
                if (bit != consumer.books.end()) {
                    const auto &b = bit->second;
                    clock = b.clock;
                    for (int sd = 0; sd < 2; ++sd)
                        for (const auto &[units, l] : b.side[sd]) {
                            const long double integral =
                                l.integral + (b.observing && clock > l.since ? (long double)l.size * (clock - l.since) : 0);
                            if (l.size > 0) {
                                ++live;
                                add["live"].push_back({clock - l.runStart, double(integral)});
                            } else if (integral > 0) {
                                ++removed;
                                add["removed"].push_back({sd, units, double(integral)});
                            }
                        }
                    add["cursor"] = {b.observing, clock - b.obsSince, b.covered};
                }
                for (const auto &run : consumer.runs)
                    if (run.end >= committed) {
                        ++tail;
                        add["tail"].push_back({run.isBid, run.units, clock - run.start, clock - run.end, run.size});
                    }
                for (const auto &[key, m] : consumer.minutes) {
                    json levels = json::array();
                    for (const auto &[isBid, units, integral] : m.integrals) levels.push_back({isBid, units, double(integral)});
                    pendingLevels += m.integrals.size();
                    add["pending"].push_back({key.second, m.eventObservedMs, m.flags, std::move(levels)});
                }
                consumer.runs.clear();
                if (truth) t.compareAnchor(state);
                const auto addCbor = json::to_cbor(add);
                anchors.push_back({{"clock", clock}, {"committedMs", committed}, {"liveLevels", live},
                                   {"recorderLevels", levels}, {"tailRuns", tail}, {"removedWithIntegral", removed},
                                   {"pendingMinutes", consumer.minutes.size()}, {"pendingMinuteLevels", pendingLevels},
                                   {"addRawBytes", live * 16 + removed * 17 + tail * 33 + pendingLevels * 17 + 24},
                                   {"addCborBytes", addCbor.size()}, {"addZstdBytes", zstdSize(addCbor)},
                                   {"recorderStateBytes", state.size()}, {"recorderStateZstdBytes", zstdSize(state)}});
                return state;
            };
        }
        if (truth) {
            o.onFeed = [&](sentinel::roller::JournalFeed &f) {
                auto snapshot = f.onSnapshot;
                auto updates = f.onUpdates;
                auto invalid = f.onInvalid;
                f.onInvalid = [&t, invalid](int64_t at, const std::string &reason) { ++t.feedInvalids; invalid(at, reason); };
                f.onUpdates = [&, updates](int64_t at, int64_t local, std::vector<Level> levels) {
                    if (dropRecorder && ++t.updateMessages == dropRecorder) { ++t.droppedRecorderUpdates; return; }
                    if (dropRecorderAt && local >= dropRecorderAt && !t.droppedRecorderUpdates) {
                        ++t.droppedRecorderUpdates;
                        t.droppedRecorderLevels = levels.size();
                        return;
                    }
                    updates(at, local, std::move(levels));
                };
                f.onSnapshot = [&, snapshot](int64_t at, int64_t local, std::vector<Level> levels) {
                    t.snapshot(recorder, consumer, at, day, levels);
                    snapshot(at, local, std::move(levels));
                };
            };
            o.onApplied = [&t](const sentinel::roller::JournalRecord &in) { t.record(in); };
        }
        o.onRecorder = [&](recording::BookRecorder *r, const sentinel::roller::JournalFeed *, int64_t) {
            if (r) { recorder = r; return; }
            if (!recorder) return;
            const auto s = recorder->stats(); // the day loop drained it at its final fence
            recorderStats = {{"columnsWritten", s.columnsWritten}, {"lateEvents", s.lateEvents},
                             {"backwardSteps", s.backwardSteps}, {"invalidations", s.invalidations},
                             {"diskErrors", s.diskErrors}};
            workerNs = threadCpuNs("book-recorder");
            recorder = nullptr;
        };

        const double cpu0 = processCpuMs();
        const auto wall0 = std::chrono::steady_clock::now();
        auto report = sentinel::roller::roll(o);
        if (truth) t.indep.finish();
        const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
        const double cpuMs = processCpuMs() - cpu0;

        json result = {{"product", product}, {"day", argv[4]}, {"mode", mode}, {"wallMs", wallMs},
                       {"processCpuMs", cpuMs}, {"workerCpuMs", workerNs / 1e6}, {"recorderStats", recorderStats},
                       {"roll", report}};
        if (truth) result["truth"] = t.report();
        if (consumerMode) {
            uint64_t afterDay = 0, inDay = 0;
            for (const auto &[key, m] : consumer.minutes) (key.second >= day + kDay ? afterDay : inDay)++;
            const auto &c = consumer.counts;
            result["events"] = {{"reset", c.resets}, {"level", c.levels}, {"applied", c.applied},
                                {"invalid", c.invalid}, {"invalidUpstream", c.upstreamInvalid},
                                {"minute", c.minutes}, {"offGridPrices", c.offGrid}, {"outsideDomainPrices", c.outsideDomain}, {"runsClosed", c.runsClosed},
                                {"suspensions", c.suspensions}, {"resumptions", c.resumptions},
                                {"validations", c.validations}};
            result["rowCheck"] = rows.json();
            result["unverifiedMinutes"] = {{"beforeDay", c.minutesNotStored}, {"afterDay", afterDay}, {"inDay", inDay}};
            result["bookCheck"] = {{"anchors", bookChecks}, {"mismatches", bookMismatches},
                                   {"levelsCompared", levelsCompared}, {"first", firstBookError}};
            result["consumerErrors"] = consumer.errors.size();
            if (!consumer.errors.empty()) result["firstConsumerError"] = consumer.errors.front();
            result["anchors"] = anchors;
        }
        std::ofstream(argv[6]) << result.dump(1) << '\n';
        std::cout << result.dump().substr(0, 2000) << '\n';
        const bool pass = !consumerMode || ((!truth || t.clean()) &&
            rows.clean() && !bookMismatches && consumer.errors.empty() && result["unverifiedMinutes"]["inDay"] == 0);
        return pass ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << "recorder_sink_replay: " << e.what() << '\n';
        return 1;
    }
}
