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
        if (r.last > 0)
            end.push_back({r.isBid, r.bin, r.last.convert_to<int64_t>()});
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

#include "levelstore/HaltRecord.hpp"
#include "levelstore/LevelLog.hpp"
TEST(LevelRoll, MalformedSourcePreservesLegacyInvalidationAndDetectsLegacyAcceptance) {
    std::vector<std::pair<json, bool>> cases;
    for (const auto *text : {"-1", "", "abc"})
        cases.push_back({level("bid", "99999.99", text), false});
    cases.push_back({level("bid", "-1", "1"), false});
    auto numeric = level("bid", "99999.99", "1");
    numeric["new_quantity"] = 1;
    cases.push_back({numeric, false});
    auto missing = level("bid", "99999.99", "1");
    missing.erase("new_quantity");
    cases.push_back({missing, false});
    for (const auto *text : {"1e0", "1junk"})
        cases.push_back({level("bid", "99999.99", text), true});
    for (const auto &[bad, divergence] : cases) {
        SCOPED_TRACE(bad.dump());
        TempDir t;
        capture::WriterConfig config;
        config.root = QString::fromStdString((t.path / "raw").string());
        config.fsyncBlocks = 0;
        {
            capture::Writer writer(config, metadata());
            const auto append = [&](int64_t ms, std::string data) {
                writer.append({capture::Kind::Frame, {ms * 1000000, ms * 1000000}, 1, std::move(data)});
            };
            append(Epoch - 60000, frame(1, "snapshot", fullBook(0)));
            append(Epoch + 1000, frame(2, "update", json::array({bad})));
            append(Epoch + 4 * Minute, frame(3, "snapshot", fullBook(0)));
            writer.close();
        }
        RollOptions c{t.path / "raw", t.path / "combined", "BTC-USD", Epoch, Epoch + 3 * Minute};
        c.levelLogRoot = t.path / "log";
        ASSERT_NO_THROW(roll(c));
        EXPECT_EQ(bool(readHalt(c.levelLogRoot, "BTC-USD")), divergence);
        auto plain = c;
        plain.outputRoot = t.path / "plain";
        plain.levelLogRoot.clear();
        ASSERT_NO_THROW(roll(plain));
        EXPECT_EQ(hmc2Files(c.outputRoot), hmc2Files(plain.outputRoot));
        if (!divergence) {
            size_t clears = 0;
            LevelLogReader(c.levelLogRoot, "BTC-USD").read([&](const auto &record) {
                if (auto b = std::get_if<MinuteBlock>(&record.value))
                    for (const auto &m : b->messages)
                        clears += m.clear == ClearCause::Invalid;
            });
            EXPECT_GT(clears, 0u);
        }
    }
}

TEST(LevelRoll, ExactSourceTracksEachFilesHeaderIncrementsAndOriginalFields) {
    JournalFeed feed(kProduct);
    feed.retainExactSource = true;
    std::vector<recording::ExactSourcePtr> sources;
    feed.onSnapshot = [&](auto, auto, auto) { sources.push_back(feed.takeExactSource()); };
    feed.onUpdates = [&](auto, auto, auto) { sources.push_back(feed.takeExactSource()); };
    for (int i = 0; i < 2; ++i) {
        JournalRecord r{{capture::Kind::Frame,
                         {(Epoch + i * 1000) * 1000000, 0},
                         1,
                         frame(i + 1, i ? "update" : "snapshot",
                               json::array({level("bid", i ? "98.123" : "99.00", i ? "0.0001" : "1.00")}))},
                        {kProduct, "run", uint64_t(i + 5), uint32_t(i)},
                        metadata()["product_metadata"],
                        false,
                        1};
        r.file = i ? "second.rawl2" : "first.rawl2";
        if (i) {
            r.metadata["quote_increment"] = "0.001";
            r.metadata["base_increment"] = "0.0001";
        }
        feed.apply(r);
    }
    ASSERT_EQ(sources.size(), 2);
    EXPECT_EQ(sources[0]->priceIncrement, "0.01");
    EXPECT_EQ(sources[1]->priceIncrement, "0.001");
    EXPECT_EQ(sources[1]->sizeIncrement, "0.0001");
    EXPECT_EQ(sources[1]->file, "second.rawl2");
    EXPECT_EQ(sources[1]->block, 6);
    EXPECT_EQ(sources[1]->record, 1);
    EXPECT_EQ(sources[1]->sequence, 2);
    EXPECT_EQ(sources[1]->levels[0].priceText, "98.123");
    EXPECT_EQ(sources[1]->levels[0].sizeText, "0.0001");
    EXPECT_EQ(sources[1]->levels[0].price.value, (ExactDecimal{98123, 3}));
    EXPECT_EQ(sources[1]->levels[0].size.value, (ExactDecimal{1, 4}));
}
