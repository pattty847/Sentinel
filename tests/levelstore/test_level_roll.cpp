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
#include <mutex>

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

// A durable anchor restore has no C state in E1b-1: C refuses, says so, and stays
// unobserved until a valid exact snapshot; never a silent fresh start. Midnight
// (refused during warmup: the day starts empty and unobserved) and intraday (the
// output records the refusal's clear at the checkpoint clock).
TEST(LevelRoll, DurableAnchorRestoreRefusesCUntilTheNextValidSnapshot) {
    for (const bool intraday : {false, true}) {
        TempDir tmp;
        writeFixture(tmp.path / "raw");
        DayCapture first, resumed;
        auto o = options(tmp.path / "raw", tmp.path / "out");
        o.fromMs = Epoch;
        o.toMs = Epoch + 4 * Hour;
        o.levelOutput = &first;
        bool stop = false;
        o.cancelled = [&] { return stop; };
        o.onApplied = [&](const JournalRecord &r) { stop = stop || r.record.time.systemNs / 1'000'000 >= Epoch + 2 * Hour + 20'000; };
        roll(o);
        // Keep only the newest anchor (intraday) or only midnight.
        std::vector<fs::path> anchors;
        for (const auto &e : fs::recursive_directory_iterator(tmp.path / "out"))
            if (e.path().extension() == ".anchor") anchors.push_back(e.path());
        std::sort(anchors.begin(), anchors.end());
        ASSERT_GE(anchors.size(), 3u);
        for (size_t i = 0; i < anchors.size(); ++i)
            if (intraday ? i + 1 != anchors.size() : i != 0) fs::remove(anchors[i]);
        o.cancelled = {};
        o.onApplied = {};
        o.useAnchors = true;
        o.levelOutput = &resumed;
        std::vector<LevelRecorder::Stats> stats;
        o.onLevelRecorder = [&](LevelRecorder *c) { if (c) stats.push_back(c->stats()); };
        const auto report = roll(o);
        ASSERT_TRUE(report["days"][0].contains("anchorBoundaryMs")) << report.dump();
        EXPECT_EQ(report["days"][0]["anchorBoundaryMs"].get<int64_t>() > Epoch, intraday) << report.dump();
        ASSERT_FALSE(stats.empty());
        EXPECT_EQ(stats.front().restoreRefusals, 1u);
        ASSERT_EQ(resumed.days.size(), 1u);
        const auto &day = resumed.days.begin()->second;
        ASSERT_FALSE(day.blocks.empty());
        if (!intraday) {
            ASSERT_TRUE(day.image);
            EXPECT_EQ(*day.image, (BookImage{Epoch, false, {}}));
        } else {
            EXPECT_FALSE(day.image); // the resumed instance does not restart the day
            ASSERT_FALSE(day.blocks.front().messages.empty());
            EXPECT_EQ(day.blocks.front().messages.front().clear, ClearCause::RestoreRefused);
        }
        bool recovered = false;
        for (const auto &blk : day.blocks) {
            for (const auto &m : blk.messages) {
                if (m.timeMs < Epoch + 3 * Hour + 2 * Minute) EXPECT_FALSE(m.observed);
                else if (m.reset) {
                    EXPECT_TRUE(m.observed);
                    recovered = true;
                    break;
                }
            }
            if (recovered) break;
            EXPECT_TRUE(blk.changes.empty()); // nothing tracked while unobserved
        }
        EXPECT_TRUE(recovered);
    }
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
