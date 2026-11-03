// Architecture C inside roll(): one LevelRecorder per day's history recorder (day
// recreation, midnight, warmup), JournalFeed's exact decimals, the in-process
// checkpoint at every anchor and at adversarial records, durable anchor restores,
// and the unsupported-price log line. QtCore only for the capture fixture writer.
#include "LevelTestSupport.hpp"
#include "capture/RawCapture.hpp"
#include "roller/Roller.hpp"
#include <gtest/gtest.h>
#include <QString>
#include <QtGlobal>
#include <fstream>
#include <map>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace sentinel;
using namespace sentinel::roller;
using namespace levelstore_test;
using nlohmann::json;

namespace {
constexpr int64_t Epoch = 1'798'761'600'000; // 2027-01-01 UTC
constexpr int64_t Minute = 60'000, Hour = 3'600'000, Day = 86'400'000;
const std::string kProduct = "BTC-USD";

json metadata() {
    return {{"product_metadata", {{"product_id", kProduct}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}};
}
json level(const char *side, const std::string &price, const std::string &qty) {
    return {{"side", side}, {"price_level", price}, {"new_quantity", qty}};
}
std::string frame(uint64_t sequence, const char *type, json updates) {
    return json({{"channel", "l2_data"}, {"sequence_num", sequence},
                 {"events", json::array({{{"type", type}, {"product_id", kProduct}, {"updates", std::move(updates)}}})}})
        .dump();
}
json fullBook(int variant) {
    return json::array({level("bid", "99999.99", "2"), level("offer", "100000.01", "3"),
                        level("bid", "99998", std::to_string(1 + variant)), level("offer", "100001.5", "0.25"),
                        level("bid", "99990.10", "0")}); // a zero-size level is not a snapshot level
}

// Records from Epoch-30 min to Epoch+Day+45 min every 5 s, with same-millisecond
// pairs, invalid periods at 3 h and 22 h 30, resets and an optional unsupported
// price at 2 h.
struct Fixture {
    std::vector<int64_t> snapshotMs, invalidMs;
    int64_t unsupportedMs = -1;
    uint64_t unsupportedSequence = 0;
};
Fixture writeFixture(const fs::path &root, bool unsupported = false) {
    Fixture f;
    capture::WriterConfig c;
    c.root = QString::fromStdString(root.string());
    c.fsyncBlocks = 0;
    capture::Writer w(c, metadata());
    uint64_t seq = 0;
    const auto at = [](int64_t ms) { return capture::Stamp{(Epoch + ms) * 1'000'000, (ms + Day) * 1'000'000}; };
    w.append({capture::Kind::TransportUp, at(-30 * Minute), 1, "{}"});
    const auto snap = [&](int64_t ms, int variant) {
        w.append({capture::Kind::Frame, at(ms), 1, frame(++seq, "snapshot", fullBook(variant))});
        f.snapshotMs.push_back(ms);
    };
    snap(-20 * Minute, 0);
    for (int64_t t = -20 * Minute + 5'000; t <= Day + 45 * Minute; t += 5'000) {
        if (t == 3 * Hour || t == Day - 90 * Minute) {
            w.append({capture::Kind::BookInvalidated, at(t), 1, json({{"product", kProduct}, {"reason", "test"}}).dump()});
            f.invalidMs.push_back(t);
            continue;
        }
        if (t == 3 * Hour + 2 * Minute || t == Day - 88 * Minute || t == Day - 10 * Minute || t == Day + 20 * Minute) {
            snap(t, int(t / Hour % 3));
            continue;
        }
        const int i = int(((t / 5'000) % 1000 + 1000) % 1000);
        const std::string price = unsupported && t == 2 * Hour ? "99999.999" : std::to_string(99'990 + i % 7) + ".5";
        if (unsupported && t == 2 * Hour) {
            f.unsupportedMs = t;
            f.unsupportedSequence = seq + 1;
        }
        w.append({capture::Kind::Frame, at(t), 1,
                  frame(++seq, "update", json::array({level("bid", price, std::to_string(i % 4) + ".125"),
                                                     level("offer", "100002", i % 3 ? "1.5" : "0")}))});
        if (i % 5 == 0) // a second message in the same millisecond
            w.append({capture::Kind::Frame, at(t), 1,
                      frame(++seq, "update", json::array({level("bid", price, std::to_string(i % 4 + 1) + ".5")}))});
    }
    w.close();
    return f;
}

// C output per day (keyed by the binding's floor).
struct DayCapture final : LevelOutput {
    std::mutex m;
    std::map<int64_t, Capture> days;
    void onDayStart(const DayBinding &b, const BookImage &i) override {
        std::lock_guard l(m);
        days[b.floorMs].onDayStart(b, i);
    }
    void onBlock(const DayBinding &b, const MinuteBlock &block) override {
        std::lock_guard l(m);
        days[b.floorMs].onBlock(b, block);
    }
};
std::map<std::string, std::string> hmc2Files(const fs::path &root) {
    std::map<std::string, std::string> out;
    for (const auto &e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".hmc2") {
            std::ifstream in(e.path(), std::ios::binary);
            out[fs::relative(e.path(), root).string()] = {std::istreambuf_iterator<char>(in), {}};
        }
    return out;
}
// Day 1 from 22:00 (warmup from the snapshot at -20 min), day 2 to 00:30.
RollOptions options(const fs::path &raw, const fs::path &out) {
    RollOptions o{raw, out, kProduct, Epoch + Day - 2 * Hour, Epoch + Day + 30 * Minute};
    o.useAnchors = false;
    o.writeAnchors = true;
    return o;
}
} // namespace

TEST(LevelRoll, JournalFeedAttachesExactDecimalsWithoutChangingTheParse) {
    JournalFeed feed(kProduct);
    std::vector<recording::Level> snap, upd;
    uint64_t seqInCallback = 0;
    feed.onSnapshot = [&](int64_t, int64_t, std::vector<recording::Level> l) { snap = std::move(l); seqInCallback = feed.sequence(); };
    feed.onUpdates = [&](int64_t, int64_t, std::vector<recording::Level> l) { upd = std::move(l); };
    const auto rec = [&](std::string payload, uint32_t record) {
        return JournalRecord{{capture::Kind::Frame, {Epoch * 1'000'000, 0}, 1, std::move(payload)},
                             {kProduct, "run", 0, record}, metadata()["product_metadata"], false, 1};
    };
    feed.apply(rec(frame(7, "snapshot", json::array({level("offer", "100000.010", "3"), level("bid", "99999.99", "2"),
                                                     level("bid", "99990.10", "0"), level("bid", "1e5", "1"),
                                                     level("offer", "100000.0200000000001", "1")})),
                   0));
    ASSERT_EQ(snap.size(), 4u); // the zero-size level is dropped, as before
    EXPECT_EQ(seqInCallback, 7u);
    // Bids first in input order, then asks: the same doubles as before plus the text.
    EXPECT_TRUE(snap[0].isBid);
    EXPECT_EQ(snap[0].price, 99999.99);
    EXPECT_EQ(std::tuple(uint64_t(snap[0].decimal.digits), uint64_t(snap[0].decimal.fraction)), std::tuple(9999999u, 2u));
    EXPECT_EQ(snap[1].price, 100000.0); // strtod accepts "1e5"; the text is not a plain decimal
    EXPECT_FALSE(snap[1].decimal.exact);
    EXPECT_EQ(std::tuple(uint64_t(snap[2].decimal.digits), uint64_t(snap[2].decimal.fraction)), std::tuple(10000001u, 2u));
    EXPECT_FALSE(snap[3].decimal.exact); // 19 significant digits
    feed.apply(rec(frame(8, "update", json::array({level("bid", "99999.99", "0"), level("offer", "100000.015", "4")})), 1));
    ASSERT_EQ(upd.size(), 2u);
    EXPECT_EQ(upd[0].size, 0);
    EXPECT_TRUE(upd[0].decimal.exact);
    EXPECT_EQ(std::tuple(uint64_t(upd[1].decimal.digits), uint64_t(upd[1].decimal.fraction)), std::tuple(100000015u, 3u));
    EXPECT_EQ(feed.sequence(), 8u);
}

// Day recreation and midnight: each day gets its own LevelRecorder; day 2's warmup
// rebuilds exactly the book day 1's output had at midnight. In-process restores at
// every anchor and at adversarial records give exactly the uninterrupted output and
// leave HMC2 identical to a run without C.
TEST(LevelRoll, DayRecreationAndInProcessRestoresMatchTheUninterruptedRun) {
    TempDir tmp;
    const auto fix = writeFixture(tmp.path / "raw");
    DayCapture a, b;
    auto oa = options(tmp.path / "raw", tmp.path / "a");
    oa.levelOutput = &a;
    roll(oa);
    auto on = options(tmp.path / "raw", tmp.path / "null");
    roll(on);
    auto ob = options(tmp.path / "raw", tmp.path / "b");
    ob.levelOutput = &b;
    std::vector<json> restores;
    int64_t lastSnapshotRecord = -1;
    uint64_t records = 0;
    ob.restoreAtAnchorForTest = [](int64_t) { return true; };
    ob.restoreAfterRecordForTest = [&](const JournalRecord &r) {
        ++records;
        const int64_t t = r.record.time.systemNs / 1'000'000 - Epoch;
        const bool snapshot = r.record.payload.find("\"snapshot\"") != std::string::npos;
        const bool afterSnapshot = lastSnapshotRecord >= 0 && records == uint64_t(lastSnapshotRecord) + 1;
        if (snapshot) lastSnapshotRecord = int64_t(records);
        return snapshot || afterSnapshot ||                      // a reset and right after it
               (t > Day - 90 * Minute && t < Day - 88 * Minute) || // inside the invalid period
               (t < Day - 2 * Hour && records % 211 == 0) ||      // warmup
               (t >= Day - 6'000 && t < Day) ||                   // the last records before midnight
               (t >= Day - 10 * Minute - 1 && t < Day && records % 3 == 0) || // day-2 warmup overlap
               t % Minute == 30'000 || records % 97 == 0;         // mid-minute and spread
    };
    ob.onInProcessRestoreForTest = [&](const json &j) { restores.push_back(j); };
    roll(ob);
    ASSERT_EQ(a.days.size(), 2u);
    ASSERT_EQ(b.days.size(), 2u);
    EXPECT_EQ(a.days.begin()->first, Epoch + Day - 2 * Hour);
    for (const auto &[floor, day] : a.days) {
        ASSERT_TRUE(day.image) << floor;
        EXPECT_EQ(day.dayStarts, 1);
        EXPECT_EQ(day.bytes(), b.days[floor].bytes()) << "day " << floor;
        EXPECT_EQ(b.days[floor].dayStarts, 1);
        EXPECT_GE(day.blocks.front().bucketStartMs, floor);
    }
    EXPECT_GT(restores.size(), 150u);
    size_t anchors = 0, failed = 0;
    for (const auto &r : restores) {
        anchors += r.at("boundaryMs").get<int64_t>() >= 0;
        failed += !r.at("levelImported").get<bool>();
    }
    EXPECT_GE(anchors, 10u); // day 1's quarters from 22:00, day 2's midnight and quarters
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(hmc2Files(tmp.path / "a"), hmc2Files(tmp.path / "null"));
    EXPECT_EQ(hmc2Files(tmp.path / "b"), hmc2Files(tmp.path / "null"));
    // Midnight: day 1 ends at its ceiling and day 2's own warmup reaches day 1's book.
    const auto &d1 = a.days.at(Epoch + Day - 2 * Hour), &d2 = a.days.at(Epoch + Day);
    EXPECT_EQ(d1.blocks.back().bucketStartMs, Epoch + Day - Minute);
    EXPECT_EQ(d2.image->atMs, Epoch + Day);
    MinuteSweep s;
    s.start(*d1.image);
    std::vector<BinRow> rows;
    int64_t covered = 0;
    for (const auto &blk : d1.blocks) s.apply(blk, rows, covered);
    std::vector<BookLevel> end;
    for (const auto &r : rows)
        if (r.last > 0) end.push_back({r.isBid, r.bin, double(r.last)});
    EXPECT_EQ(end, d2.image->levels);
    EXPECT_EQ(s.observed(), d2.image->observed);
    // The invalid period is a clear, and nothing is observed until the next snapshot.
    bool sawClear = false;
    for (const auto &blk : d1.blocks)
        for (const auto &m : blk.messages)
            if (m.clear == ClearCause::Invalid) {
                sawClear = true;
                EXPECT_EQ(m.timeMs, Epoch + fix.invalidMs.back());
            }
    EXPECT_TRUE(sawClear);
}

// Durable HMC2 progress alone says nothing about C's output prefix.
TEST(LevelRoll, DurableResumeWithoutPairedCStateIsRefusedBeforeOutput) {
    TempDir tmp;
    writeFixture(tmp.path / "raw");
    auto o = options(tmp.path / "raw", tmp.path / "out");
    o.fromMs = Epoch;
    o.toMs = Epoch + 20 * Minute;
    roll(o); // existing HMC2-only checkpoints and anchors
    const auto files = hmc2Files(tmp.path / "out");
    for (const bool useAnchors : {false, true}) {
        DayCapture resumed;
        o.useAnchors = useAnchors;
        o.levelOutput = &resumed;
        EXPECT_THROW(roll(o), std::runtime_error);
        EXPECT_TRUE(resumed.days.empty());
        EXPECT_EQ(hmc2Files(tmp.path / "out"), files);
    }
    // A separate anchor cache is also insufficient, even with an empty output root.
    o.anchorRoot = tmp.path / "out";
    o.outputRoot = tmp.path / "fresh";
    DayCapture fresh;
    o.levelOutput = &fresh;
    EXPECT_THROW(roll(o), std::runtime_error);
    EXPECT_TRUE(fresh.days.empty());
    // Explicit full replay into fresh output remains available.
    o.useAnchors = false;
    o.writeAnchors = false;
    EXPECT_NO_THROW(roll(o));
    EXPECT_FALSE(fresh.days.empty());
}

TEST(LevelRoll, OutputFailureAbortsWithoutReportingSuccessfulProgress) {
    TempDir tmp;
    writeFixture(tmp.path / "raw");
    for (const bool image : {false, true}) for (const bool afterAccept : {false, true}) {
        SCOPED_TRACE(testing::PrintToString(std::make_pair(image, afterAccept)));
        Capture output;
        output.failDayStart = image;
        output.failNextBlock = !image;
        output.failAfterAccept = afterAccept;
        auto o = options(tmp.path / "raw", tmp.path / (std::to_string(image) + std::to_string(afterAccept)));
        o.fromMs = Epoch;
        o.toMs = Epoch + 20 * Minute;
        o.levelOutput = &output;
        int commits = 0;
        o.onCommitted = [&](int64_t) { ++commits; };
        EXPECT_THROW(roll(o), std::runtime_error);
        EXPECT_EQ(commits, 0);
        EXPECT_EQ(output.dayStarts, 1);
        EXPECT_EQ(output.blockCalls, image ? 0 : 1);
        EXPECT_FALSE(fs::exists(o.outputRoot / kProduct / "roller.json"));
    }
}

TEST(LevelRoll, InternalFailureCannotBeSwallowedAsAnOptionalAnchorFailure) {
    TempDir tmp;
    writeFixture(tmp.path / "raw");
    DayCapture output;
    auto o = options(tmp.path / "raw", tmp.path / "out");
    o.fromMs = Epoch;
    o.toMs = Epoch + 20 * Minute;
    o.levelOutput = &output;
    recording::BookRecorder *recorder = nullptr;
    LevelRecorder *levelRecorder = nullptr;
    bool injected = false;
    int commitsAfterFailure = 0, anchorsAfterFailure = 0;
    std::string checkpointBefore;
    const auto checkpoint = o.outputRoot / kProduct / "roller.json";
    const auto readCheckpoint = [&] {
        std::ifstream in(checkpoint);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    o.onRecorder = [&](auto *r, const auto *, int64_t) { recorder = r; };
    o.onLevelRecorder = [&](auto *c) { levelRecorder = c; };
    o.onApplied = [&](const JournalRecord &r) {
        if (injected || !recorder || r.record.time.systemNs / 1'000'000 < Epoch + 15 * Minute) return;
        recorder->drain();
        // A deterministic internal invariant fault at the anchor boundary.
        levelRecorder->onApplied(kProduct, Epoch - 1, true, true);
        ASSERT_EQ(levelRecorder->stats().failures, 1u);
        injected = true;
        checkpointBefore = readCheckpoint();
    };
    o.onCommitted = [&](int64_t) { commitsAfterFailure += injected; };
    o.afterAnchorForTest = [&](int64_t) { anchorsAfterFailure += injected; };
    EXPECT_THROW(roll(o), std::runtime_error);
    EXPECT_TRUE(injected);
    EXPECT_EQ(anchorsAfterFailure, 0);
    EXPECT_EQ(commitsAfterFailure, 0);
    EXPECT_EQ(readCheckpoint(), checkpointBefore);
}

namespace {
std::mutex logMutex;
std::vector<std::string> logLines;
void captureLog(QtMsgType, const QMessageLogContext &, const QString &msg) {
    std::lock_guard l(logMutex);
    logLines.push_back(msg.toStdString());
}
} // namespace

TEST(LevelRoll, UnsupportedPriceIsLoggedOncePerEpisodeWithItsSourceAndCounted) {
    TempDir tmp;
    const auto fix = writeFixture(tmp.path / "raw", true);
    DayCapture out;
    auto o = options(tmp.path / "raw", tmp.path / "out");
    o.fromMs = Epoch;
    o.toMs = Epoch + 150 * Minute;
    o.levelOutput = &out;
    LevelRecorder::Stats last;
    LevelRecorder *current = nullptr;
    o.onLevelRecorder = [&](LevelRecorder *c) {
        if (!c && current) last = current->stats(); // before it is destroyed, recorder drained
        current = c;
    };
    logLines.clear();
    const auto previous = qInstallMessageHandler(captureLog);
    roll(o);
    qInstallMessageHandler(previous);
    EXPECT_EQ(last.unsupportedEpisodes, 1u);
    EXPECT_GE(last.unsupportedPrices, 1u);
    std::vector<std::string> warnings;
    for (const auto &l : logLines)
        if (l.find("levelstore: unsupported price") != std::string::npos) warnings.push_back(l);
    ASSERT_EQ(warnings.size(), 1u);
    const auto &w = warnings.front();
    EXPECT_NE(w.find("symbol=BTC-USD"), std::string::npos) << w;
    EXPECT_NE(w.find("decimal=99999.999"), std::string::npos) << w;
    EXPECT_NE(w.find("identity=finer-than-scale"), std::string::npos) << w;
    EXPECT_NE(w.find("exchangeMs=" + std::to_string(Epoch + fix.unsupportedMs)), std::string::npos) << w;
    EXPECT_NE(w.find("sequence=" + std::to_string(fix.unsupportedSequence)), std::string::npos) << w;
    // The episode lasts until the next valid exact snapshot (none before 2 h 30 here).
    const auto &day = out.days.begin()->second;
    for (const auto &blk : day.blocks)
        for (const auto &m : blk.messages)
            if (m.timeMs >= Epoch + fix.unsupportedMs) EXPECT_FALSE(m.observed);
}

// The warning carries the original text of an unsupported
// price, also when it is not a plain decimal (exponent, trailing garbage that
// strtod ignores, too many significant digits). Each recovers at a valid exact
// snapshot, so each starts its own episode and logs once.
TEST(LevelRoll, UnsupportedPriceLogKeepsTheOriginalText) {
    TempDir tmp;
    Capture out;
    LevelRecorder c(DayBinding{kProduct, Epoch, Epoch + Day, 2}, &out);
    recording::RecorderConfig cfg = deriveGrid(metadata()["product_metadata"], 100000).config(tmp.path);
    cfg.sink = &c;
    recording::BookRecorder recorder(cfg);
    JournalFeed feed(kProduct);
    feed.onSnapshot = [&](int64_t e, int64_t l, std::vector<recording::Level> v) {
        recorder.onSnapshotAt(kProduct, e, l, std::move(v), feed.sequence(), feed.takePriceText());
    };
    feed.onUpdates = [&](int64_t e, int64_t l, std::vector<recording::Level> v) {
        recorder.onUpdatesAt(kProduct, e, l, std::move(v), feed.sequence(), feed.takePriceText());
    };
    uint64_t seq = 0;
    uint32_t record = 0;
    const auto apply = [&](int64_t ms, const char *type, json updates) {
        feed.apply(JournalRecord{{capture::Kind::Frame, {(Epoch + ms) * 1'000'000, 0}, 1, frame(++seq, type, std::move(updates))},
                                 {kProduct, "run", 0, record++}, metadata()["product_metadata"], false, 1});
    };
    const std::vector<std::string> texts = {"1e5", "99999.5abc", "99999.500000000000000001"};
    logLines.clear();
    const auto previous = qInstallMessageHandler(captureLog);
    apply(1'000, "snapshot", fullBook(0));
    int64_t t = 2'000;
    for (const auto &text : texts) {
        apply(t, "update", json::array({level("bid", text, "1")}));
        apply(t + 500, "snapshot", fullBook(0)); // recovers: the next text starts a new episode
        t += 1'000;
    }
    recorder.drainForTest();
    qInstallMessageHandler(previous);
    std::vector<std::string> warnings;
    for (const auto &l : logLines)
        if (l.find("levelstore: unsupported price") != std::string::npos) warnings.push_back(l);
    ASSERT_EQ(warnings.size(), texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        EXPECT_NE(warnings[i].find("decimal=" + texts[i] + " "), std::string::npos) << warnings[i];
        EXPECT_NE(warnings[i].find("identity=no-decimal-text"), std::string::npos) << warnings[i];
    }
    EXPECT_EQ(c.stats().unsupportedEpisodes, texts.size());
}

namespace {
std::string frameFor(const std::string &product, uint64_t sequence, const char *type, json updates) {
    return json({{"channel", "l2_data"}, {"sequence_num", sequence},
                 {"events", json::array({{{"type", type}, {"product_id", product}, {"updates", std::move(updates)}}})}})
        .dump();
}
json metadataFor(const std::string &product) {
    return {{"product_id", product}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}};
}
// The first sink event (the snapshot's reset) blocks until released: the worker
// consumes nothing while the producers run.
struct Gate final : recording::RecorderSink {
    recording::RecorderSink &next;
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    explicit Gate(recording::RecorderSink &n) : next(n) {}
    void release() {
        { std::lock_guard l(m); open = true; }
        cv.notify_all();
    }
    void onReset(const std::string &s, int64_t c) noexcept override {
        std::unique_lock l(m);
        cv.wait(l, [&] { return open; });
        l.unlock();
        next.onReset(s, c);
    }
    void onImprecise(const std::string &s, int64_t c, const LevelChange &x) noexcept override { next.onImprecise(s, c, x); }
    void onLevel(const std::string &s, int64_t c, const LevelChange &x) noexcept override { next.onLevel(s, c, x); }
    void onApplied(const std::string &s, int64_t c, bool v, bool o) noexcept override { next.onApplied(s, c, v, o); }
    void onInvalid(const std::string &s, int64_t c, const std::string &r, bool u) noexcept override { next.onInvalid(s, c, r, u); }
    void onMinute(const std::string &s, int64_t b, uint32_t o, uint32_t f) noexcept override { next.onMinute(s, b, o, f); }
};
std::vector<std::string> unsupportedWarnings() {
    std::lock_guard l(logMutex);
    std::vector<std::string> out;
    for (const auto &line : logLines)
        if (line.find("levelstore: unsupported price") != std::string::npos) out.push_back(line);
    return out;
}
} // namespace

// The text lives with its recorder message, so it survives any
// number of unsupported prices in one message, consumers that run late, several
// producers, and overlong text (head and tail kept, the length stated).
TEST(LevelRoll, UnsupportedPriceTextSurvivesBigMessagesDelayedConsumersAndLength) {
    TempDir tmp;
    logLines.clear();
    const auto previous = qInstallMessageHandler(captureLog);
    struct Lane {
        std::string product;
        std::unique_ptr<Capture> out = std::make_unique<Capture>();
        std::unique_ptr<LevelRecorder> c;
        std::unique_ptr<Gate> gate;
        std::unique_ptr<recording::BookRecorder> r;
        std::unique_ptr<JournalFeed> feed;
        std::string firstText;
    };
    std::vector<Lane> lanes(2);
    lanes[0].product = "BTC-USD";
    lanes[1].product = "ETH-USD";
    for (size_t i = 0; i < lanes.size(); ++i) {
        auto &lane = lanes[i];
        lane.c = std::make_unique<LevelRecorder>(DayBinding{lane.product, Epoch, Epoch + Day, 2}, lane.out.get());
        lane.gate = std::make_unique<Gate>(*lane.c);
        auto cfg = deriveGrid(metadataFor(lane.product), 100000).config(tmp.path / lane.product);
        cfg.sink = lane.gate.get();
        lane.r = std::make_unique<recording::BookRecorder>(cfg);
        lane.feed = std::make_unique<JournalFeed>(lane.product);
        auto *r = lane.r.get();
        auto *f = lane.feed.get();
        const auto product = lane.product;
        f->onSnapshot = [r, f, product](int64_t e, int64_t l, std::vector<recording::Level> v) {
            r->onSnapshotAt(product, e, l, std::move(v), f->sequence(), f->takePriceText());
        };
        f->onUpdates = [r, f, product](int64_t e, int64_t l, std::vector<recording::Level> v) {
            r->onUpdatesAt(product, e, l, std::move(v), f->sequence(), f->takePriceText());
        };
    }
    // Lane 0: one message with 1,100 unsupported prices (more than the old ring held),
    // then 300 more messages. Lane 1: 700 messages, the first with an overlong text.
    // Both workers are blocked until every producer has finished.
    lanes[0].firstText = "99999.5first";
    lanes[1].firstText = "99999." + std::string(600, '7') + "TAIL";
    std::vector<std::thread> producers;
    for (size_t i = 0; i < lanes.size(); ++i)
        producers.emplace_back([&, i] {
            auto &lane = lanes[i];
            uint64_t seq = 0;
            uint32_t record = 0;
            const auto apply = [&](int64_t ms, const char *type, json updates) {
                lane.feed->apply(JournalRecord{{capture::Kind::Frame, {(Epoch + ms) * 1'000'000, 0}, 1,
                                                frameFor(lane.product, ++seq, type, std::move(updates))},
                                               {lane.product, "run", 0, record++}, metadataFor(lane.product), false, 1});
            };
            apply(1'000, "snapshot", fullBook(0));
            json first = json::array({level("bid", lane.firstText, "1")});
            if (i == 0)
                for (int k = 1; k < 1'100; ++k) first.push_back(level("bid", "99999.5other" + std::to_string(k), "1"));
            apply(2'000, "update", first);
            for (int k = 0; k < (i == 0 ? 300 : 699); ++k)
                apply(3'000 + k, "update", json::array({level("bid", "99998.5x" + std::to_string(k), "1")}));
        });
    for (auto &t : producers) t.join();
    for (auto &lane : lanes) lane.gate->release();
    for (auto &lane : lanes) lane.r->drainForTest();
    qInstallMessageHandler(previous);
    const auto warnings = unsupportedWarnings();
    ASSERT_EQ(warnings.size(), 2u); // one episode per product
    for (const auto &lane : lanes) {
        const auto w = std::find_if(warnings.begin(), warnings.end(),
                                    [&](const std::string &x) { return x.find("symbol=" + lane.product) != std::string::npos; });
        ASSERT_NE(w, warnings.end()) << lane.product;
        const auto expected = recording::diagnosticPriceText(lane.firstText);
        EXPECT_NE(w->find("decimal=" + expected + " "), std::string::npos) << *w;
        EXPECT_EQ(lane.c->stats().unsupportedEpisodes, 1u);
        EXPECT_EQ(lane.c->stats().unsupportedPrices, lane.product == "BTC-USD" ? 1'100u + 300u : 700u);
    }
    EXPECT_NE(warnings[0].find("TAIL") + warnings[1].find("TAIL"), 2 * std::string::npos); // the suffix is kept
}

// A snapshot lists levels in any order, but the recorder sees all
// bids before all asks. The kept text is the one of the first unsupported level in
// that final order, so the once-per-episode warning has it.
TEST(LevelRoll, SnapshotDiagnosticFollowsTheRecorderLevelOrder) {
    TempDir tmp;
    Capture out;
    LevelRecorder c(DayBinding{kProduct, Epoch, Epoch + Day, 2}, &out);
    recording::RecorderConfig cfg = deriveGrid(metadata()["product_metadata"], 100000).config(tmp.path);
    cfg.sink = &c;
    recording::BookRecorder recorder(cfg);
    JournalFeed feed(kProduct);
    feed.onSnapshot = [&](int64_t e, int64_t l, std::vector<recording::Level> v) {
        recorder.onSnapshotAt(kProduct, e, l, std::move(v), feed.sequence(), feed.takePriceText());
    };
    logLines.clear();
    const auto previous = qInstallMessageHandler(captureLog);
    feed.apply(JournalRecord{{capture::Kind::Frame, {(Epoch + 1'000) * 1'000'000, 0}, 1,
                              frame(1, "snapshot", json::array({level("offer", "100001.5ask", "1"), level("offer", "100002", "1"),
                                                                level("bid", "99999", "2"), level("bid", "99998.5bid", "1")}))},
                             {kProduct, "run", 0, 0}, metadata()["product_metadata"], false, 1});
    recorder.drainForTest();
    qInstallMessageHandler(previous);
    const auto warnings = unsupportedWarnings();
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("decimal=99998.5bid "), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("side=bid"), std::string::npos) << warnings[0];
}
