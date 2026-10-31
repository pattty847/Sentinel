// Offline evidence for Architecture C E1b-1 (never part of ctest). Read-only on RAWL2;
// everything it writes is under OUT_ROOT.
//
//   levelstore_replay RAW_ROOT OUT_ROOT PRODUCT YYYY-MM-DD null|level|restore REPORT.json
//     null     production roll() of one UTC day, no sink (HMC2 baseline).
//     level    the same with C (LevelRecorder) attached, uninterrupted. C's output goes to
//              OUT_ROOT/level.bin. Every HMC2 minute row is checked against C's own timeline
//              (twap and peak codes, observed time, flags, underflow). At every anchor C's
//              state is exported, imported into a scratch instance and re-exported.
//     restore  the same, but at every anchor and at adversarial records (mid-minute, the last
//              records before midnight and the day end, inside invalid periods, warmup, right
//              after snapshot resets) the recorder and C are exported to bytes, destroyed and
//              re-imported in process before replay resumes.
//     cost     C attached with its output discarded (no file, no row check): the
//              recorder worker's CPU with C, against null, for paired cost runs.
//   levelstore_replay compare A_OUT B_OUT REPORT.json
//     Compares two level.bin files completely: the day image, every minute block (messages
//     and changes), every derived per-level run (start, end, size), every observation
//     interval and every native per-minute row (integral, presence, peak, last). Exact.
// The process exit code is informational; harnesses gate on the JSON "pass" field.
#include "LevelTestSupport.hpp"
#include "roller/Roller.hpp"
#include "servermodel/RecordingCodec.hpp"
#include <QCoreApplication>
#include <mach/mach.h>
#include <sys/resource.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>

using namespace recording;
using namespace levelstore_test;
using nlohmann::json;

namespace {
constexpr int64_t kDay = 86'400'000, kMinute = 60'000, kQuarter = 900'000;

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
json summary(std::vector<double> v) {
    if (v.empty()) return {{"n", 0}};
    std::sort(v.begin(), v.end());
    const auto q = [&](double f) { return v[size_t(f * double(v.size() - 1) + 0.5)]; };
    return {{"n", v.size()}, {"min", v.front()}, {"p50", q(0.5)}, {"p99", q(0.99)}, {"max", v.back()},
            {"mean", std::accumulate(v.begin(), v.end(), 0.0) / double(v.size())}};
}

// ------------------------------------------------------------------ block file
// [u8 tag][varint length][payload]: tag 1 binding+image, 2 block.
struct BlockFile final : LevelOutput {
    std::ofstream out;
    std::vector<char> buffer = std::vector<char>(8 << 20);
    std::function<void(const DayBinding &, const BookImage &)> onImage;
    std::function<void(const MinuteBlock &)> onMinute;
    uint64_t images = 0, blocks = 0, bytes = 0;
    explicit BlockFile(const fs::path &path) {
        out.rdbuf()->pubsetbuf(buffer.data(), std::streamsize(buffer.size()));
        out.open(path, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + path.string());
    }
    void put(uint8_t tag, const std::vector<uint8_t> &payload) {
        ByteWriter w;
        w.u8(tag);
        w.varint(payload.size());
        out.write(reinterpret_cast<const char *>(w.bytes.data()), std::streamsize(w.bytes.size()));
        out.write(reinterpret_cast<const char *>(payload.data()), std::streamsize(payload.size()));
        bytes += w.bytes.size() + payload.size();
    }
    void onDayStart(const DayBinding &b, const BookImage &image) override {
        ByteWriter w;
        w.str(b.symbol);
        w.svarint(b.floorMs);
        w.svarint(b.ceilingMs);
        w.svarint(b.priceDigits);
        encodeImage(w, image);
        put(1, w.bytes);
        ++images;
        if (onImage) onImage(b, image);
    }
    void onBlock(const DayBinding &, const MinuteBlock &block) override {
        ByteWriter w;
        encodeBlock(w, block);
        put(2, w.bytes);
        ++blocks;
        if (onMinute) onMinute(block);
    }
};
struct BlockReader {
    std::vector<uint8_t> data;
    size_t pos = 0;
    explicit BlockReader(const fs::path &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + path.string());
        data.assign(std::istreambuf_iterator<char>(in), {});
    }
    // false at the end.
    bool next(uint8_t &tag, std::span<const uint8_t> &payload) {
        if (pos == data.size()) return false;
        ByteReader r(std::span<const uint8_t>(data).subspan(pos));
        tag = r.u8();
        const uint64_t n = r.varint();
        const size_t header = data.size() - pos - r.remaining();
        if (n > r.remaining()) throw std::runtime_error("truncated block file");
        payload = std::span<const uint8_t>(data).subspan(pos + header, size_t(n));
        pos += header + size_t(n);
        return true;
    }
};

// ------------------------------------------------------------------ HMC2 row check
// C's own timeline, binned into each layer's rows the way the recorder bins them,
// against the committed HMC2 minute record: twap and peak codes, observed time,
// flags and underflow, every row of every minute.
struct RowCheck {
    uint64_t records = 0, rows = 0, twapExact = 0, twapOffByOne = 0, twapOther = 0, peakExact = 0, peakOffByOne = 0,
             peakOther = 0, zeroNonzero = 0, missingEntries = 0, observedMismatches = 0, coveredMismatches = 0,
             flagMismatches = 0, underflowRecords = 0, underflowExplained = 0, underflowTie = 0, underflowUnexplained = 0,
             underflowMissing = 0, unmatchedRecords = 0, episodeMinutes = 0;
    std::vector<std::string> first;
    void note(const std::string &s) { if (first.size() < 12) first.push_back(s); }
    static int code(long double v, const SizeScale &scale) {
        if (v <= 0) return 0;
        return encodeSize(static_cast<double>(std::min(v, static_cast<long double>(std::numeric_limits<double>::max()))), scale);
    }
    void check(const Hmc2Record &r, const MinuteBlock &block, const std::vector<BinRow> &cRows, int64_t covered,
               bool cEpisode) {
        ++records;
        const auto at = r.header.layer + "@" + std::to_string(r.bucketStartMs);
        if (r.observedMs != block.recorderObservedMs) { ++observedMismatches; note("observed " + at); }
        if (cEpisode) ++episodeMinutes;
        else if (covered != int64_t(r.observedMs)) {
            ++coveredMismatches;
            note("C covered " + std::to_string(covered) + " != " + std::to_string(r.observedMs) + " " + at);
        }
        if ((r.flags & ~kUnderflow) != block.recorderFlags) { ++flagMismatches; note("flags " + at); }
        const auto inWindow = [&](int64_t row, bool ask) {
            return ask ? row >= r.askRowLo && row <= r.askRowHi : row >= r.bidRowLo && row <= r.bidRowHi;
        };
        std::map<std::pair<int64_t, bool>, const BinRow *> mine;
        for (const auto &row : cRows) mine[{row.bin, !row.isBid}] = &row;
        const double floor = r.header.sizeScale.floor;
        bool expectUnderflow = false, tie = false;
        for (const auto &e : r.entries) {
            ++rows;
            const auto it = mine.find({e.row, e.isAsk});
            const long double twap = it == mine.end() || !r.observedMs ? 0 : it->second->integral / r.observedMs;
            const long double peak = it == mine.end() ? 0 : it->second->peak;
            for (const auto &[v, c, kind] : {std::tuple(twap, int(e.twapCode & kMaxCode), 0),
                                             std::tuple(peak, int(e.peakCode & kMaxCode), 1)}) {
                if (v > 0 && v < floor) expectUnderflow = true;
                if (c == 1 && v >= floor && v <= floor * (1 + 1e-9)) tie = true;
                if ((v > 0) != (c > 0)) {
                    ++zeroNonzero;
                    note(std::string(kind ? "peak" : "twap") + " zero/nonzero " + at + " row=" + std::to_string(e.row));
                    continue;
                }
                const int d = std::abs(code(v, r.header.sizeScale) - c);
                auto &exact = kind ? peakExact : twapExact, &one = kind ? peakOffByOne : twapOffByOne,
                     &other = kind ? peakOther : twapOther;
                if (d == 0) ++exact;
                else if (d == 1) ++one;
                else {
                    ++other;
                    note(std::string(kind ? "peak" : "twap") + " code " + at + " row=" + std::to_string(e.row) +
                         " hmc2=" + std::to_string(c) + " c=" + std::to_string(code(v, r.header.sizeScale)));
                }
            }
        }
        for (const auto &[key, row] : mine) {
            if (!inWindow(key.first, key.second) || !(row->integral > 0 || row->peak > 0)) continue;
            const bool present = std::any_of(r.entries.begin(), r.entries.end(),
                                             [&](const Hmc2Entry &e) { return e.row == key.first && e.isAsk == key.second; });
            if (!present) { ++missingEntries; note("missing " + at + " row=" + std::to_string(key.first)); }
        }
        const bool hmc2 = r.flags & kUnderflow;
        underflowRecords += hmc2;
        if (hmc2 && expectUnderflow) ++underflowExplained;
        else if (hmc2 && tie) ++underflowTie;
        else if (hmc2) { ++underflowUnexplained; note("underflow unexplained " + at); }
        else if (expectUnderflow) { ++underflowMissing; note("underflow missing " + at); }
    }
    bool clean() const {
        return !twapOther && !peakOther && !zeroNonzero && !missingEntries && !observedMismatches && !coveredMismatches &&
               !flagMismatches && !underflowUnexplained && !underflowMissing && !unmatchedRecords;
    }
    json report() const {
        return {{"records", records}, {"rows", rows}, {"twapExact", twapExact}, {"twapOffByOne", twapOffByOne},
                {"twapOther", twapOther}, {"peakExact", peakExact}, {"peakOffByOne", peakOffByOne}, {"peakOther", peakOther},
                {"zeroNonzero", zeroNonzero}, {"missingEntries", missingEntries}, {"observedMismatches", observedMismatches},
                {"coveredMismatches", coveredMismatches}, {"episodeMinutes", episodeMinutes},
                {"flagMismatches", flagMismatches}, {"underflowRecords", underflowRecords},
                {"underflowExplained", underflowExplained}, {"underflowFloorTie", underflowTie},
                {"underflowUnexplained", underflowUnexplained}, {"underflowMissing", underflowMissing},
                {"unmatchedRecords", unmatchedRecords}, {"first", first}};
    }
};

// Sweeps C's output per HMC2 layer as it is emitted (recorder worker thread) and
// checks each committed HMC2 minute (same thread, later) against it.
struct LayerSweeps {
    std::optional<BookImage> image;
    std::vector<MinuteBlock> pending; // until both layers' row widths are known
    std::map<std::string, int64_t> width;
    std::map<std::string, std::unique_ptr<MinuteSweep>> sweeps;
    struct Minute {
        MinuteBlock block;
        std::map<std::string, std::pair<std::vector<BinRow>, int64_t>> layers;
        bool episode = false;
    };
    std::map<int64_t, Minute> minutes;
    RowCheck check;
    bool inEpisode = false;
    uint64_t messages = 0, changes = 0, resets = 0, damaged = 0;
    std::map<std::string, uint64_t> clears;
    void sweep(const MinuteBlock &b) {
        // Minutes in which C was unobserved by its own decision (an unsupported price,
        // a failure or a refused restore) at any time: its covered is legitimately lower.
        Minute m{b, {}, inEpisode};
        for (const auto &msg : b.messages) {
            ++messages;
            resets += msg.reset;
            if (msg.reset) inEpisode = false;
            if (msg.clear != ClearCause::None) {
                ++clears[clearCauseName(msg.clear)];
                if (msg.clear != ClearCause::Invalid) m.episode = inEpisode = true;
            }
        }
        m.episode = m.episode || b.damaged;
        changes += b.changes.size();
        damaged += b.damaged;
        for (auto &[name, s] : sweeps) {
            auto &[rows, covered] = m.layers[name];
            s->apply(b, rows, covered);
        }
        minutes[b.bucketStartMs] = std::move(m);
    }
    bool ready() const { return width.size() == 2 && image; }
    void start() {
        for (const auto &[name, w] : width) {
            sweeps[name] = std::make_unique<MinuteSweep>(w);
            sweeps[name]->start(*image);
        }
        for (const auto &b : pending) sweep(b);
        pending.clear();
    }
    void onBlock(const MinuteBlock &b) {
        if (!sweeps.empty()) sweep(b);
        else pending.push_back(b);
    }
    std::vector<std::shared_ptr<const Hmc2Record>> pendingRecords; // until ready()
    void onRecord(std::shared_ptr<const Hmc2Record> record) {
        const Hmc2Record &r = *record;
        if (r.header.tfMs != kMinute) return;
        if (!width.contains(r.header.layer)) width[r.header.layer] = r.header.rowTickUnits;
        if (!ready()) {
            pendingRecords.push_back(std::move(record));
            return;
        }
        if (sweeps.empty()) {
            start();
            auto held = std::move(pendingRecords);
            pendingRecords.clear();
            for (auto &h : held) checkRecord(*h);
        }
        checkRecord(r);
    }
    void checkRecord(const Hmc2Record &r) {
        const auto it = minutes.find(r.bucketStartMs);
        if (it == minutes.end()) { ++check.unmatchedRecords; check.note("unmatched " + r.header.layer + "@" + std::to_string(r.bucketStartMs)); return; }
        const auto layer = it->second.layers.find(r.header.layer);
        if (layer == it->second.layers.end()) { ++check.unmatchedRecords; return; }
        check.check(r, it->second.block, layer->second.first, layer->second.second, it->second.episode);
        it->second.layers.erase(layer);
        if (it->second.layers.empty()) minutes.erase(it);
    }
};

// ------------------------------------------------------------------ compare
struct Streams {
    // Derived from one block file: runs, observation intervals and native rows per block.
    struct Run {
        bool isBid;
        int64_t units, start, end;
        uint64_t startMessage, endMessage; // global message ordinals
        double size;
        bool operator==(const Run &) const = default;
    };
    struct Interval {
        int64_t start, end;
        uint64_t startMessage, endMessage;
        bool operator==(const Interval &) const = default;
    };
    std::map<std::pair<bool, int64_t>, std::tuple<int64_t, uint64_t, double>> open; // start, message, size
    bool observed = false;
    int64_t obsStart = 0;
    uint64_t obsMessage = 0, ordinal = 0;
    MinuteSweep sweep{1};
    void start(const BookImage &image) {
        for (const auto &l : image.levels) open[{l.isBid, l.units}] = {image.atMs, 0, l.size};
        observed = image.observed;
        obsStart = image.atMs;
        sweep.start(image);
    }
    void block(const MinuteBlock &b, std::vector<Run> &runs, std::vector<Interval> &intervals, std::vector<BinRow> &rows,
               int64_t &covered) {
        runs.clear();
        intervals.clear();
        size_t c = 0;
        for (uint32_t i = 0; i < b.messages.size(); ++i) {
            const auto &m = b.messages[i];
            const uint64_t ord = ++ordinal;
            const auto end = [&](const std::pair<bool, int64_t> &k, std::tuple<int64_t, uint64_t, double> &v) {
                runs.push_back({k.first, k.second, std::get<0>(v), m.timeMs, std::get<1>(v), ord, std::get<2>(v)});
            };
            if (m.clear != ClearCause::None) {
                for (auto &[k, v] : open) end(k, v);
                open.clear();
            }
            for (; c < b.changes.size() && b.changes[c].message == i; ++c) {
                const auto &ch = b.changes[c];
                const std::pair k{ch.isBid, ch.units};
                if (const auto it = open.find(k); it != open.end()) {
                    end(k, it->second);
                    open.erase(it);
                }
                if (ch.size > 0) open[k] = {m.timeMs, ord, ch.size};
            }
            if (m.observed != observed) {
                if (observed) intervals.push_back({obsStart, m.timeMs, obsMessage, ord});
                observed = m.observed;
                obsStart = m.timeMs;
                obsMessage = ord;
            }
        }
        sweep.apply(b, rows, covered);
    }
};
template <class T> uint64_t diffRows(const std::vector<T> &a, const std::vector<T> &b) {
    uint64_t d = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d += !(a[i] == b[i]);
    return d;
}
json compare(const fs::path &aDir, const fs::path &bDir) {
    BlockReader a(aDir / "level.bin"), b(bDir / "level.bin");
    Streams sa, sb;
    uint64_t blocks = 0, blockDiffs = 0, messages = 0, messageDiffs = 0, changes = 0, changeDiffs = 0, runs = 0,
             runDiffs = 0, intervals = 0, intervalDiffs = 0, rows = 0, rowDiffs = 0, coveredDiffs = 0, imageLevels = 0,
             imageDiffs = 0, headerDiffs = 0, nonzeroPeakRows = 0, presenceRows = 0;
    std::vector<std::string> first;
    const auto note = [&](const std::string &s) { if (first.size() < 12) first.push_back(s); };
    std::vector<Streams::Run> ra, rb;
    std::vector<Streams::Interval> ia, ib;
    std::vector<BinRow> wa, wb;
    for (;;) {
        uint8_t ta = 0, tb = 0;
        std::span<const uint8_t> pa, pb;
        const bool ha = a.next(ta, pa), hb = b.next(tb, pb);
        if (!ha && !hb) break;
        if (ha != hb || ta != tb) {
            ++blockDiffs;
            note("record stream length/type differs");
            break;
        }
        ByteReader ya(pa), yb(pb);
        if (ta == 1) {
            const auto bind = [](ByteReader &r) {
                DayBinding d;
                d.symbol = r.str(256);
                d.floorMs = r.svarint();
                d.ceilingMs = r.svarint();
                d.priceDigits = int(r.svarint());
                return d;
            };
            const auto da = bind(ya), db = bind(yb);
            const auto xa = decodeImage(ya), xb = decodeImage(yb);
            headerDiffs += !(da == db);
            imageLevels += xa.levels.size();
            imageDiffs += diffRows(xa.levels, xb.levels) + (xa.atMs != xb.atMs) + (xa.observed != xb.observed);
            sa.start(xa);
            sb.start(xb);
            continue;
        }
        const auto ba = decodeBlock(ya), bb = decodeBlock(yb);
        ++blocks;
        const bool header = ba.bucketStartMs == bb.bucketStartMs && ba.recorderObservedMs == bb.recorderObservedMs &&
                            ba.recorderFlags == bb.recorderFlags && ba.observedAtStart == bb.observedAtStart &&
                            ba.damaged == bb.damaged;
        if (!header) { ++headerDiffs; note("block header " + std::to_string(ba.bucketStartMs)); }
        messages += ba.messages.size();
        changes += ba.changes.size();
        const auto md = diffRows(ba.messages, bb.messages), cd = diffRows(ba.changes, bb.changes);
        messageDiffs += md;
        changeDiffs += cd;
        if (md || cd) note("block " + std::to_string(ba.bucketStartMs) + " messages " + std::to_string(md) + " changes " + std::to_string(cd));
        blockDiffs += !(ba == bb);
        int64_t ca = 0, cb = 0;
        sa.block(ba, ra, ia, wa, ca);
        sb.block(bb, rb, ib, wb, cb);
        runs += ra.size();
        intervals += ia.size();
        rows += wa.size();
        for (const auto &r : wa) {
            nonzeroPeakRows += r.peak > 0;
            presenceRows += r.presenceMs > 0;
        }
        const auto rd = diffRows(ra, rb), id = diffRows(ia, ib), wd = diffRows(wa, wb);
        runDiffs += rd;
        intervalDiffs += id;
        rowDiffs += wd;
        coveredDiffs += ca != cb;
        if (rd || id || wd) note("derived " + std::to_string(ba.bucketStartMs) + " runs " + std::to_string(rd) +
                                 " intervals " + std::to_string(id) + " rows " + std::to_string(wd));
    }
    const uint64_t differing = blockDiffs + headerDiffs + imageDiffs + messageDiffs + changeDiffs + runDiffs +
                               intervalDiffs + rowDiffs + coveredDiffs;
    return {{"pass", differing == 0 && blocks > 0}, {"differingRows", differing},
            {"compared", {{"blocks", blocks}, {"imageLevels", imageLevels}, {"messages", messages}, {"changes", changes},
                          {"runsClosedInDay", runs}, {"observationIntervalsClosedInDay", intervals},
                          {"nativeMinuteRows", rows}, {"nativeRowsWithPeak", nonzeroPeakRows},
                          {"nativeRowsWithPresence", presenceRows}}},
            {"diffs", {{"blocks", blockDiffs}, {"headers", headerDiffs}, {"image", imageDiffs}, {"messages", messageDiffs},
                       {"changes", changeDiffs}, {"runs", runDiffs}, {"intervals", intervalDiffs},
                       {"nativeRows", rowDiffs}, {"covered", coveredDiffs}}},
            {"first", first}};
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 5 && std::string(argv[1]) == "compare") {
            json result;
            try {
                result = compare(argv[2], argv[3]);
            } catch (const std::exception &e) { // a stream that cannot be read is a difference, reported
                result = {{"pass", false}, {"differingRows", nullptr}, {"error", e.what()}};
            }
            std::ofstream(argv[4]) << result.dump(1) << '\n';
            std::cout << result.dump() << '\n';
            return result.at("pass").get<bool>() ? 0 : 3;
        }
        if (argc != 7) {
            std::cerr << "usage: levelstore_replay RAW OUT PRODUCT YYYY-MM-DD null|level|restore|cost REPORT.json\n"
                         "       levelstore_replay compare A_OUT B_OUT REPORT.json\n";
            return 2;
        }
        const fs::path raw = fs::canonical(argv[1]), out = argv[2];
        const std::string product = argv[3], mode = argv[5];
        if (mode != "null" && mode != "level" && mode != "restore" && mode != "cost") throw std::runtime_error("bad mode");
        if (fs::exists(out) && !fs::is_empty(out)) throw std::runtime_error("OUT_ROOT must be new or empty");
        fs::create_directories(out / "hmc2");
        const auto source = raw.string() + "/", dest = fs::canonical(out).string() + "/";
        if (dest.starts_with(source) || source.starts_with(dest)) throw std::runtime_error("roots overlap");
        const int64_t day = sentinel::roller::parseTime(argv[4]);

        sentinel::roller::RollOptions o;
        o.journalRoot = raw;
        o.outputRoot = out / "hmc2";
        o.product = product;
        o.fromMs = day;
        o.toMs = day + kDay;
        o.useAnchors = false; // fresh root; anchors are written (quarter checkpoints)
        o.writeAnchors = true;

        std::unique_ptr<BlockFile> file;
        LayerSweeps layers;
        LevelRecorder *current = nullptr;
        LevelRecorder::Stats finalStats;
        json recorderStats;
        int64_t workerNs = -1;
        std::vector<double> blobBytes, exportMs, importMs, memory, liveLevels, openMessages;
        uint64_t roundTripMismatches = 0, scratchRefusals = 0;
        json restores = json::array();
        std::map<std::string, uint64_t> restoreKinds;
        std::string pendingKind;
        uint64_t restoreFailures = 0;
        struct Discard final : LevelOutput {
            void onDayStart(const DayBinding &, const BookImage &) override {}
            void onBlock(const DayBinding &, const MinuteBlock &) override {}
        } discard;
        if (mode == "cost") {
            o.levelOutput = &discard;
            o.onLevelRecorder = [&](LevelRecorder *c) {
                if (!c && current) finalStats = current->stats();
                current = c;
            };
        }
        if (mode == "level" || mode == "restore") {
            file = std::make_unique<BlockFile>(out / "level.bin");
            file->onImage = [&](const DayBinding &, const BookImage &image) { layers.image = image; };
            file->onMinute = [&](const MinuteBlock &b) { layers.onBlock(b); };
            o.levelOutput = file.get();
            o.onLevelRecorder = [&](LevelRecorder *c) {
                if (!c && current) finalStats = current->stats();
                current = c;
            };
            o.publisher = [&](std::shared_ptr<const Hmc2Record> r) { layers.onRecord(std::move(r)); };
        }
        if (mode == "level") {
            // At every anchor (drained): C's export, an import into a scratch instance
            // and its re-export must reproduce the blob byte for byte.
            o.anchorExportForTest = [&](BookRecorder &r) {
                auto bytes = r.exportState();
                if (!current || bytes.empty()) return bytes;
                const auto clock = r.checkpointClock(product);
                const auto t0 = std::chrono::steady_clock::now();
                const auto blob = current->exportState(clock, bytes);
                const auto t1 = std::chrono::steady_clock::now();
                struct Null final : LevelOutput {
                    void onDayStart(const DayBinding &, const BookImage &) override {}
                    void onBlock(const DayBinding &, const MinuteBlock &) override {}
                } null;
                LevelRecorder scratch(current->binding(), &null);
                const auto imported = scratch.importState(blob, clock, bytes);
                const auto t2 = std::chrono::steady_clock::now();
                if (!imported.ok) ++scratchRefusals;
                else if (scratch.exportState(clock, bytes) != blob) ++roundTripMismatches;
                blobBytes.push_back(double(blob.size()));
                exportMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                importMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
                memory.push_back(double(current->memoryBytes()));
                liveLevels.push_back(double(current->liveLevels()));
                openMessages.push_back(double(current->openBlock().messages.size()));
                return bytes;
            };
        }
        // Adversarial in-process restore points (restore mode).
        bool insideInvalid = false, afterSnapshot = false;
        int invalidRestores = 0;
        int64_t nextMid = day + kQuarter / 2 + 30'000; // hh:07:30, hh:22:30, ...
        uint64_t warmupRecords = 0;
        if (mode == "restore") {
            o.onFeed = [&](sentinel::roller::JournalFeed &f) {
                auto invalid = f.onInvalid;
                auto snapshot = f.onSnapshot;
                f.onInvalid = [&, invalid](int64_t t, const std::string &reason) {
                    if (!insideInvalid) invalidRestores = 0;
                    insideInvalid = true;
                    invalid(t, reason);
                };
                f.onSnapshot = [&, snapshot](int64_t e, int64_t l, std::vector<Level> levels) {
                    insideInvalid = false;
                    afterSnapshot = true;
                    snapshot(e, l, std::move(levels));
                };
            };
            o.restoreAtAnchorForTest = [&](int64_t) {
                pendingKind = "anchor";
                return true;
            };
            o.restoreAfterRecordForTest = [&](const sentinel::roller::JournalRecord &r) {
                const int64_t t = r.record.time.systemNs / 1'000'000;
                pendingKind.clear();
                if (afterSnapshot) pendingKind = "afterSnapshotReset";
                else if (insideInvalid && invalidRestores < 3) { pendingKind = "insideInvalid"; ++invalidRestores; }
                else if ((t >= day - 2'000 && t < day) || (t >= day + kDay - 2'000 && t < day + kDay)) pendingKind = "lastBeforeMidnight";
                else if (t < day && (++warmupRecords % 1500 == 1)) pendingKind = "warmup";
                else if (t >= nextMid && t < day + kDay) {
                    pendingKind = "midMinute";
                    while (nextMid <= t) nextMid += kQuarter;
                }
                afterSnapshot = false;
                return !pendingKind.empty();
            };
            o.onInProcessRestoreForTest = [&](const json &j) {
                ++restoreKinds[pendingKind];
                if (!j.at("levelImported").get<bool>()) ++restoreFailures;
                blobBytes.push_back(j.at("levelBytes").get<double>());
                exportMs.push_back(j.at("levelExportMs").get<double>());
                importMs.push_back(j.at("levelImportMs").get<double>());
                memory.push_back(j.at("levelMemoryBytes").get<double>());
                auto row = j;
                row["kind"] = pendingKind;
                restores.push_back(std::move(row));
            };
        }
        o.onRecorder = [&](BookRecorder *r, const sentinel::roller::JournalFeed *, int64_t) {
            static BookRecorder *last = nullptr;
            if (r) { last = r; return; }
            if (!last) return;
            const auto s = last->stats();
            recorderStats = {{"columnsWritten", s.columnsWritten}, {"lateEvents", s.lateEvents},
                             {"backwardSteps", s.backwardSteps}, {"invalidations", s.invalidations},
                             {"diskErrors", s.diskErrors}};
            workerNs = threadCpuNs("book-recorder");
            last = nullptr;
        };

        const double cpu0 = processCpuMs();
        const auto wall0 = std::chrono::steady_clock::now();
        const auto report = sentinel::roller::roll(o);
        const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
        if (file) file->out.flush();
        json result = {{"product", product}, {"day", argv[4]}, {"mode", mode}, {"wallMs", wallMs},
                       {"processCpuMs", processCpuMs() - cpu0}, {"workerCpuMs", workerNs / 1e6},
                       {"recorderStats", recorderStats}, {"roll", report}};
        bool pass = recorderStats.value("diskErrors", 1) == 0;
        if (mode == "cost")
            result["level"] = {{"stats", {{"messages", finalStats.messages}, {"changes", finalStats.changes},
                                          {"blocks", finalStats.blocks}, {"failures", finalStats.failures}}}};
        if (file) {
            const auto &s = finalStats;
            result["level"] = {{"images", file->images}, {"blocks", file->blocks}, {"fileBytes", file->bytes},
                               {"stats", {{"messages", s.messages}, {"changes", s.changes}, {"blocks", s.blocks},
                                          {"resets", s.resets}, {"clears", s.clears},
                                          {"unsupportedPrices", s.unsupportedPrices},
                                          {"unsupportedEpisodes", s.unsupportedEpisodes}, {"failures", s.failures},
                                          {"restoreRefusals", s.restoreRefusals}, {"foreignEvents", s.foreignEvents}}},
                               {"timeline", {{"messages", layers.messages}, {"changes", layers.changes},
                                             {"resets", layers.resets}, {"clears", layers.clears},
                                             {"damagedBlocks", layers.damaged}}}};
            result["rowCheck"] = layers.check.report();
            result["uncheckedMinutes"] = layers.minutes.size(); // finalised but never committed (the day end)
            result["heldRecords"] = layers.pendingRecords.size();
            result["checkpoint"] = {{"blobBytes", summary(blobBytes)}, {"exportMs", summary(exportMs)},
                                    {"importMs", summary(importMs)}, {"memoryBytes", summary(memory)},
                                    {"liveLevels", summary(liveLevels)}, {"openMessages", summary(openMessages)},
                                    {"roundTripMismatches", roundTripMismatches}, {"scratchRefusals", scratchRefusals}};
            pass = pass && layers.check.clean() && s.failures == 0 && s.restoreRefusals == 0 && layers.damaged == 0 &&
                   file->images == 1 && roundTripMismatches == 0 && scratchRefusals == 0 && layers.minutes.empty() &&
                   layers.pendingRecords.empty() && layers.check.records > 0;
        }
        if (mode == "restore") {
            result["restores"] = {{"total", restores.size()}, {"byKind", restoreKinds}, {"importFailures", restoreFailures},
                                  {"each", restores}};
            pass = pass && restoreFailures == 0 && restoreKinds["anchor"] >= 96 && restoreKinds["midMinute"] > 0 &&
                   restoreKinds["lastBeforeMidnight"] > 0 && restoreKinds["warmup"] > 0 &&
                   restoreKinds["afterSnapshotReset"] > 0;
        }
        result["pass"] = pass;
        std::ofstream(argv[6]) << result.dump(1) << '\n';
        auto brief = result;
        brief.erase("roll");
        if (brief.contains("restores")) brief["restores"].erase("each");
        std::cout << brief.dump() << '\n';
        return pass ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << "levelstore_replay: " << e.what() << '\n';
        return 1;
    }
}
