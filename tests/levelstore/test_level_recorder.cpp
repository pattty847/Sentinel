#include "LevelTestSupport.hpp"
#include "levelstore/HaltRecord.hpp"
#include "levelstore/LevelLog.hpp"
#include "levelstore/SealStore.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <map>
using namespace recording;
using namespace levelstore_test;
namespace {
constexpr int64_t Epoch = kHmc2MinMs, Minute = 60000;
struct Input {
    bool bid;
    const char *price;
    const char *size;
};
struct Pipe {
    TempDir dir;
    Capture out;
    std::unique_ptr<LevelRecorder> c;
    std::unique_ptr<BookRecorder> r;
    uint64_t sequence = 0;
    explicit Pipe(bool exact = true, LevelOutput *output = nullptr, int minutes = 10) {
        if (exact)
            c = std::make_unique<LevelRecorder>(DayBinding{"BTC-USD", Epoch, Epoch + minutes * Minute, 2}, output ? output : &out);
        RecorderConfig cfg;
        cfg.root = dir.path;
        cfg.priceScale = 100;
        cfg.layers = {{"near", 100, .5, 2, false}};
        cfg.sink = c.get();
        cfg.deterministicResume = true;
        r = std::make_unique<BookRecorder>(cfg);
    }
    void input(int64_t time, bool snap, std::initializer_list<Input> entries, const char *pi = "0.01",
               const char *qi = "0.00000001") {
        auto source = std::make_shared<ExactSource>();
        source->snapshot = snap;
        source->file = "fixture.rawl2";
        source->run = "fixture";
        source->block = 7;
        source->record = 3;
        source->sequence = ++sequence;
        source->priceIncrement = pi;
        source->sizeIncrement = qi;
        std::vector<Level> levels;
        for (const auto &e : entries) {
            const auto i = static_cast<uint32_t>(source->levels.size());
            source->levels.push_back({e.bid, e.price, e.size, parseDecimal(e.price), parseDecimal(e.size)});
            levels.push_back(
                {e.bid, std::strtod(e.price, nullptr), std::strtod(e.size, nullptr), parsePriceDecimal(e.price), i});
        }
        if (snap)
            r->onSnapshotAt("BTC-USD", Epoch + time, Epoch + time, std::move(levels), sequence, {}, source);
        else
            r->onUpdatesAt("BTC-USD", Epoch + time, Epoch + time, std::move(levels), sequence, {}, source);
        r->drainForTest();
    }
    void start() { input(0, true, {{true, "99", "1"}, {false, "101", "2"}}); }
    void tick(int64_t t) {
        r->onTick(Epoch + t);
        r->drainForTest();
    }
    std::map<std::string, std::string> files() {
        std::map<std::string, std::string> result;
        for (const auto &e : fs::recursive_directory_iterator(dir.path))
            if (e.path().extension() == ".hmc2") {
                std::ifstream in(e.path(), std::ios::binary);
                result[fs::relative(e.path(), dir.path).string()] = {std::istreambuf_iterator<char>(in), {}};
            }
        return result;
    }
};
void sameHmc(Pipe &a, Pipe &b) {
    a.tick(210000);
    b.tick(210000);
    EXPECT_EQ(a.r->exportState(), b.r->exportState());
    const auto files = a.files();
    ASSERT_FALSE(files.empty());
    EXPECT_EQ(files, b.files());
}
} // namespace
TEST(LevelRecorder, RoundedPriceCollisionHaltsWithoutAFalseGap) {
    Pipe a, b(false);
    a.start();
    b.start();
    a.tick(90000);
    b.tick(90000);
    const auto committed = a.out.blocks;
    for (auto *p : {&a, &b})
        p->input(91000, true, {{true, "100.001", "1"}, {false, "100.002", "1"}});
    ASSERT_TRUE(a.c->halted());
    EXPECT_FALSE(a.c->failed());
    EXPECT_EQ(a.c->haltRecord()["reason"], "distinct exact prices share legacy key");
    for (auto *p : {&a, &b})
        p->input(92000, true, {{true, "99", "1"}, {false, "101", "2"}});
    sameHmc(a, b);
    EXPECT_EQ(a.out.blocks, committed);
    EXPECT_THROW(a.c->exportState(a.r->checkpointClock("BTC-USD"), a.r->exportState()), std::runtime_error);
    EXPECT_EQ(a.c->haltRecord()["file"], "fixture.rawl2");
    EXPECT_EQ(a.c->haltRecord()["block"], 7);
    EXPECT_EQ(a.c->haltRecord()["record"], 3);
    EXPECT_EQ(a.c->haltRecord()["sequence"], 2);
    EXPECT_EQ(a.c->haltRecord()["priceText"], "100.002");
    EXPECT_EQ(a.c->haltRecord()["sizeText"], "1");
}
TEST(LevelRecorder, ExactlyCrossedSnapshotIsAnOrdinaryInvalidation) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b})
        p->input(1000, true, {{true, "102", "1"}, {false, "101", "2"}});
    EXPECT_FALSE(a.c->halted());
    EXPECT_FALSE(a.c->observed());
    EXPECT_EQ(a.c->openBlock().messages.back().clear, ClearCause::Invalid);
    for (auto *p : {&a, &b})
        p->input(2000, true, {{true, "99", "1"}, {false, "101", "2"}});
    EXPECT_TRUE(a.c->observed());
    sameHmc(a, b);
}
TEST(LevelRecorder, InternalInvalidationOfAnExactUsableBookHalts) {
    Pipe a;
    a.start();
    a.c->onInvalid("BTC-USD", Epoch + 1000, "internal failure", false);
    ASSERT_TRUE(a.c->halted());
    EXPECT_EQ(a.c->haltRecord()["reason"], "legacy invalidation contradicts exact book");
}
TEST(LevelRecorder, UpstreamInvalidationRemainsAnObservation) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b}) {
        p->r->onInvalid("BTC-USD", Epoch + 1000, "transport down");
        p->r->drainForTest();
    }
    EXPECT_FALSE(a.c->halted());
    EXPECT_FALSE(a.c->observed());
    EXPECT_EQ(a.c->liveLevels(), 0);
    for (auto *p : {&a, &b})
        p->input(2000, true, {{true, "99", "1"}, {false, "101", "2"}});
    EXPECT_TRUE(a.c->observed());
    sameHmc(a, b);
}
TEST(LevelRecorder, SuppressedExactQuantityChangeHalts) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b})
        p->input(1000, false, {{true, "99", "1.00000000000000001"}});
    ASSERT_TRUE(a.c->halted());
    EXPECT_EQ(a.c->haltRecord()["reason"], "legacy suppressed exact quantity change");
    sameHmc(a, b);
}
TEST(LevelRecorder, UnkeyableExactPriceHalts) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b})
        p->input(1000, false, {{true, "0.001", "1"}});
    ASSERT_TRUE(a.c->halted());
    EXPECT_EQ(a.c->haltRecord()["reason"], "legacy cannot key exact price");
    sameHmc(a, b);
}
TEST(LevelRecorder, Int64OverflowHaltsOnlyTheExactConsumer) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b})
        p->input(1000, false, {{true, "99", "9223372036854775808"}});
    ASSERT_TRUE(a.c->halted());
    EXPECT_EQ(a.c->haltRecord()["reason"], "decimal mantissa exceeds int64");
    sameHmc(a, b);
}
TEST(LevelRecorder, OffGridValuesAndIncrementChangesKeepExactScales) {
    Pipe a, b(false);
    a.start();
    b.start();
    for (auto *p : {&a, &b}) {
        p->input(1000, false, {{true, "98.123", "0.000000001"}});
        p->input(2000, false, {{true, "99", "1.25"}}, "0.1", "0.05");
        p->input(3000, false, {{true, "98", "0.03"}}, "0.1", "0.05");
    }
    ASSERT_FALSE(a.c->halted());
    EXPECT_EQ(a.c->stats().offGridValues, 3);
    const auto block = a.c->openBlock();
    EXPECT_EQ(block.messages.back().priceDigits, 3);
    EXPECT_EQ(block.messages.back().sizeDigits, 9);
    const auto levels = a.c->book();
    EXPECT_NE(std::find(levels.begin(), levels.end(), BookLevel{true, 98000, 30000000}), levels.end());
    EXPECT_NE(std::find(levels.begin(), levels.end(), BookLevel{true, 98123, 1}), levels.end());
    EXPECT_NE(std::find(levels.begin(), levels.end(), BookLevel{true, 99000, 1250000000}), levels.end());
    sameHmc(a, b);
}
TEST(LevelRecorder, AllInputsSurviveWhilePeaksAreMessageAtomic) {
    Pipe a;
    a.start();
    a.input(1000, false, {{true, "99", "9"}, {true, "99", "3"}});
    a.input(1000, false, {{true, "99", "5"}});
    a.input(1000, false, {{true, "99", "2"}});
    a.input(2000, false, {{true, "99", "0"}});
    a.tick(90000);
    ASSERT_EQ(a.out.blocks.size(), 1);
    const auto &block = a.out.blocks[0];
    ASSERT_EQ(block.inputs.size(), 7);
    EXPECT_EQ(block.inputs[2].size, 900000000);
    EXPECT_EQ(block.inputs[3].size, 300000000);
    ByteWriter bytes;
    encodeBlock(bytes, block);
    ByteReader reader(bytes.bytes);
    EXPECT_EQ(decodeBlock(reader), block);
    const auto minutes = sweep(a.out);
    ASSERT_EQ(minutes.size(), 1);
    const auto it =
        std::find_if(minutes[0].rows.begin(), minutes[0].rows.end(), [](const auto &row) { return row.isBid; });
    ASSERT_NE(it, minutes[0].rows.end());
    EXPECT_EQ(it->peak, Wide(500000000));
    EXPECT_EQ(it->integral, Wide(300000000000LL));
    EXPECT_EQ(it->presenceMs, 2000);
    EXPECT_EQ(it->last, 0);
}
TEST(LevelRecorder, CheckedAggregatesExceedInt64WithoutRounding) {
    BookImage image{Epoch, true, {{true, 99, INT64_MAX}, {true, 98, INT64_MAX}, {false, 101, 1}}, 0, 0};
    MinuteBlock block;
    block.bucketStartMs = Epoch;
    block.observedAtStart = true;
    MinuteSweep sweep(100);
    sweep.start(image);
    std::vector<BinRow> rows;
    int64_t covered = 0;
    sweep.apply(block, rows, covered);
    ASSERT_EQ(covered, Minute);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].last, Wide(INT64_MAX) * 2);
    EXPECT_EQ(rows[0].integral, Wide(INT64_MAX) * 2 * Minute);
    EXPECT_THROW((Wide(1) << 126) * Wide(4), std::overflow_error);
}
TEST(LevelRecorder, PairedStateRoundTripAndCorruptionRefusal) {
    Pipe a;
    a.start();
    a.input(1000, false, {{true, "98.123", "0.000000001"}});
    a.tick(2000);
    const auto recorder = a.r->exportState();
    const auto clock = a.r->checkpointClock("BTC-USD");
    const auto blob = a.c->exportState(clock, recorder);
    Capture out;
    LevelRecorder c(a.c->binding(), &out);
    ASSERT_TRUE(c.importState(blob, clock, recorder).ok);
    EXPECT_EQ(c.book(), a.c->book());
    EXPECT_EQ(c.openBlock(), a.c->openBlock());
    EXPECT_EQ(c.exportState(clock, recorder), blob);
    for (size_t i = 0; i < blob.size(); ++i) {
        auto changed = blob;
        changed[i] ^= 1;
        LevelRecorder rejected(a.c->binding(), &out);
        EXPECT_FALSE(rejected.importState(changed, clock, recorder).ok) << i;
        EXPECT_TRUE(rejected.failed());
    }
}
TEST(LevelRecorder, TransientOutputFailureIsNotADeterministicHalt) {
    Pipe a;
    a.start();
    a.out.failNextBlock = true;
    a.tick(90000);
    EXPECT_TRUE(a.c->failed());
    EXPECT_FALSE(a.c->halted());
    EXPECT_THROW(a.c->throwIfFailed(), std::runtime_error);
}
TEST(LevelRecorder, DurableHaltRecordKeepsCheckpointAndFirstFailure) {
    TempDir dir;
    const auto cp = dir.path / "roller.json";
    {
        std::ofstream out(cp);
        out << nlohmann::json{{"boundary", 123}};
    }
    const nlohmann::json record = {{"representationVersion", 1}, {"symbol", "BTC-USD"}, {"reason", "test"}};
    writeHalt(dir.path, "BTC-USD", record, cp);
    const auto first = readHalt(dir.path, "BTC-USD");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->at("committedCheckpoint"), nlohmann::json({{"boundary", 123}}));
    auto second = record;
    second["reason"] = "later";
    writeHalt(dir.path, "BTC-USD", second, cp);
    EXPECT_EQ(readHalt(dir.path, "BTC-USD"), first);
    second["representationVersion"] = 2;
    writeHalt(dir.path, "BTC-USD", second, cp);
    EXPECT_EQ(readHalt(dir.path, "BTC-USD"), first);
    {
        std::ofstream out(dir.path / "BTC-USD/halt.json");
        out << "{}";
    }
    EXPECT_THROW(readHalt(dir.path, "BTC-USD"), std::exception);
}

TEST(LevelRecorder, EncodingFrameLimitHaltsInsteadOfRetrying) {
    Pipe a;
    a.start();
    a.out.overflowNextBlock = true;
    a.tick(90000);
    ASSERT_TRUE(a.c->halted());
    EXPECT_FALSE(a.c->failed());
    EXPECT_TRUE(a.out.blocks.empty());
    EXPECT_NE(a.c->haltRecord()["reason"].get<std::string>().find("encoding frame exceeds representation limit"), std::string::npos);
    EXPECT_NO_THROW(requireEncodingSize(512ULL * 1024 * 1024, 512ULL * 1024 * 1024));
    EXPECT_THROW(requireEncodingSize(512ULL * 1024 * 1024 + 1, 512ULL * 1024 * 1024), std::overflow_error);
}

TEST(LevelRecorder, AllocationFailureStopsWithoutHaltOrRepairGap) {
    for (const std::string site : {"changes", "clear"}) {
        SCOPED_TRACE(site);
        Pipe a, b(false);
        a.start();
        b.start();
        int faults = 0;
        a.c->failAllocationForTest = [&](const char *at) {
            if (site == at) {
                ++faults;
                return true;
            }
            return false;
        };
        if (site == "changes")
            for (auto *p : {&a, &b})
                p->input(1000, false, {{true, "99", "3"}});
        else
            for (auto *p : {&a, &b}) {
                p->r->onInvalid("BTC-USD", Epoch + 1000, "transport down");
                p->r->drainForTest();
            }
        ASSERT_TRUE(a.c->failed());
        EXPECT_FALSE(a.c->halted());
        EXPECT_EQ(faults, 1);
        for (auto *p : {&a, &b})
            p->input(2000, true, {{true, "99", "4"}, {false, "101", "2"}});
        sameHmc(a, b);
        EXPECT_EQ(faults, 1);
        EXPECT_TRUE(a.out.blocks.empty());
        EXPECT_THROW(a.c->exportState(a.r->checkpointClock("BTC-USD"), a.r->exportState()), std::runtime_error);
    }
}

TEST(LevelRecorder, EncodedHourMismatchDurablyHaltsInsteadOfFailing) {
    TempDir dir;
    struct Output : LevelOutput {
        fs::path root;
        SealStore store;
        D2Hour hour;
        explicit Output(fs::path p) : root(p), store(p, "BTC-USD") {
            store.verificationInputForTest = [](D2Hour &h) { ++h.blocks[0].inputs[0].size; };
        }
        void onDayStart(const DayBinding &, const BookImage &i) override { hour.start = i; }
        void onBlock(const DayBinding &, const MinuteBlock &b) override {
            hour.blocks.push_back(b);
            if (hour.blocks.size() == 60) store.publish(hour);
        }
        void onHalt(const nlohmann::json &record) override { writeHalt(root, "BTC-USD", record, {}); }
    } output(dir.path);
    Pipe p(true, &output, 120);
    p.start();
    for (int i = 1; i <= 61; ++i) p.input(i*Minute, false, {{true,"99","1"}});
    ASSERT_TRUE(p.c->halted());
    EXPECT_FALSE(p.c->failed());
    const auto halt = readHalt(dir.path,"BTC-USD");
    ASSERT_TRUE(halt);
    const auto reason = halt->at("reason").get<std::string>();
    EXPECT_NE(reason.find("hour=" + std::to_string(Epoch)), std::string::npos);
    EXPECT_NE(reason.find("D2 lossless verification failed"), std::string::npos);
    for (const auto *key : {"symbol","representationVersion","file","run","block","record","sequence",
                           "priceIncrement","sizeIncrement","priceDigits","sizeDigits","priceText","sizeText",
                           "committedCheckpoint"}) EXPECT_TRUE(halt->contains(key)) << key;
    EXPECT_EQ(halt->at("priceText"), "99");
    EXPECT_EQ(halt->at("sizeText"), "1");
}
TEST(LevelRecorder, EnvelopeTimestampRejectionDoesNotHalt) {
    Pipe a, b(false); a.start(); b.start();
    for (auto *p : {&a,&b}) p->input(-Epoch-1, false, {{true,"99","2"}});
    EXPECT_FALSE(a.c->halted()); EXPECT_FALSE(a.c->failed());
    EXPECT_FALSE(a.c->observed()); EXPECT_EQ(a.c->liveLevels(), 0);
    for (auto *p : {&a,&b}) p->input(2000, true, {{true,"99","1"},{false,"101","2"}});
    sameHmc(a,b);
}
TEST(LevelRecorder, MalformedAcceptedSourceUsesItsOwnHaltReason) {
    Pipe p; p.start();
    auto source = std::make_shared<ExactSource>();
    source->malformed = true; source->priceIncrement="0.01"; source->sizeIncrement="0.00000001";
    source->levels.push_back({true,"99","1e0",parseDecimal("99"),parseDecimal("1e0")});
    p.r->onUpdatesAt("BTC-USD",Epoch+1000,Epoch+1000,{{true,99,1,parsePriceDecimal("99"),0}},0,{},source);
    p.r->drainForTest();
    ASSERT_TRUE(p.c->halted());
    EXPECT_EQ(p.c->haltRecord()["reason"], "legacy accepted malformed decimal input");
}
