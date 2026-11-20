// Offline policy changes and stateful replay: all journals and output are temporary.
// Hourly expectations come from the synthetic schedule, not a second recorder.
#include "roller/Roller.hpp"
#include "roller/AnchorStore.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>
#include <cmath>
#include <map>
#include <string_view>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Minute = 60'000, Hour = 3'600'000, Day = 86'400'000;
const int64_t Epoch = parseTime("2026-10-24");
const std::string Product = "KXRECOVERY-TEST";
json spec(const std::string& step = "0.01") {
    return {{"price_min","0"},{"price_max","1"},{"size_increment","0.01"},
        {"price_ranges",json::array({{{"start","0"},{"end","1"},{"step",step}}})}};
}
json tapered() {
    auto s = spec();
    s["price_ranges"] = {{{"start","0"},{"end","0.1"},{"step","0.001"}},
        {{"start","0.1"},{"end","0.9"},{"step","0.01"}},
        {{"start","0.9"},{"end","1"},{"step","0.001"}}};
    return s;
}
json padded(json s) {
    for (const auto* key : {"price_min","price_max","size_increment"}) {
        auto value = s[key].get<std::string>();
        s[key] = "00" + value + (value.find('.') == std::string::npos ? ".0000" : "00");
    }
    for (auto& band : s["price_ranges"]) for (const auto* key : {"start","end","step"}) {
        auto value = band[key].get<std::string>();
        band[key] = "00" + value + (value.find('.') == std::string::npos ? ".0000" : "00");
    }
    return s;
}
json header(const json& s) {
    return {{"venue","kalshi"},{"product_metadata",{{"product_id",Product}}},{"instrument",s}};
}
capture::WriterConfig config(const fs::path& root) {
    capture::WriterConfig c; c.root = QString::fromStdString(root.string()); c.symbol = Product; c.fsyncBlocks = 0;
    return c;
}
json snapshot() {
    return {{"type","orderbook_snapshot"},{"sid",1},{"seq",1},
        {"msg",{{"market_ticker",Product},{"yes_dollars_fp",json::array({{"0.5600","10.00"}})}}}};
}
void append(capture::Writer& writer, int64_t t, const json& j,
            capture::Kind kind = capture::Kind::Frame) {
    writer.append({kind,{t * 1'000'000,(t - Epoch + 1) * 1'000'000},1,j.dump()});
}
void ticks(capture::Writer& writer, int first, int last) {
    for (int i = first; i <= last; ++i)
        append(writer, Epoch + i * Minute + 3000, json::object(), capture::Kind::CaptureStarted);
}
json read(const fs::path& file) { std::ifstream in(file); return json::parse(in); }
std::map<std::string,std::string> files(const fs::path& root, const std::string& day = "") {
    std::map<std::string,std::string> result;
    if (!fs::exists(root)) return result;
    for (const auto& f : fs::recursive_directory_iterator(root)) {
        if (f.path().extension() != ".hmc2" || (!day.empty() && !f.path().filename().string().starts_with(day))) continue;
        std::ifstream in(f.path(),std::ios::binary);
        result[fs::relative(f.path(),root).generic_string()] = {std::istreambuf_iterator<char>(in),{}};
    }
    return result;
}
void oneRun(const fs::path& root, const json& h, bool coinbase = false) {
    capture::Writer writer(config(root),h);
    const auto book = coinbase ? json{{"channel","l2_data"},
        {"events",json::array({{{"type","snapshot"},{"product_id",Product},
            {"updates",json::array({{{"side","bid"},{"price_level","0.55"},{"new_quantity","10"}},
                                   {{"side","offer"},{"price_level","0.65"},{"new_quantity","10"}}})}}})}} : snapshot();
    append(writer,Epoch,book); ticks(writer,1,4); writer.close();
}
}

TEST(KalshiRecovery, MiddayUnrepresentableInstrumentPreservesPrefix) {
    for (bool sizeChange : {false,true}) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        {
            capture::Writer writer(config(root / "raw"),header(spec()));
            append(writer,Epoch,snapshot()); ticks(writer,1,5); writer.close();
        }
        RollOptions o{root / "raw",root / "out",Product,Epoch,Epoch + 5 * Minute};
        roll(o); const auto prefix = files(o.outputRoot);
        ASSERT_FALSE(prefix.empty());
        const auto checkpoint = read(o.outputRoot / Product / "roller.json");
        {
            auto policy = sizeChange ? spec() : spec("0.0005");
            if (sizeChange) policy["size_increment"] = "0.001";
            capture::Writer writer(config(root / "raw"),header(policy));
            // Refuse the first record of the new policy, even before its snapshot.
            append(writer,Epoch + 5 * Minute + 5000,json::object(),capture::Kind::CaptureStarted);
            auto book = snapshot();
            book["msg"]["yes_dollars_fp"] = json::array({{"0.0550","10.00"},{"0.0570","20.00"}});
            book["msg"]["no_dollars_fp"] = json::array({{"0.9390","30.00"}});
            append(writer,Epoch + 5 * Minute + 6000,book); ticks(writer,6,10); writer.close();
        }
        o.toMs = Epoch + 10 * Minute;
        int64_t lastApplied = 0;
        o.onApplied = [&](const JournalRecord& r) { lastApplied = r.record.time.systemNs / 1'000'000; };
        try { roll(o); FAIL() << "silently accepted a changed intra-day instrument"; }
        catch (const std::runtime_error& e) { EXPECT_NE(std::string(e.what()).find("Kalshi grid:"),std::string::npos); EXPECT_NE(std::string(e.what()).find(sizeChange ? "0.01" : "0.001"),std::string::npos); }
        EXPECT_LT(lastApplied,Epoch + 5 * Minute + 5000);
        EXPECT_EQ(files(o.outputRoot),prefix);
        EXPECT_EQ(read(o.outputRoot / Product / "roller.json"),checkpoint);
    }
}

TEST(KalshiRecovery, EquivalentDecimalHeadersShareIdentityAndPersistNormalizedInstrument) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto canonical = tapered(), textual = padded(canonical);
    EXPECT_EQ(deriveKalshiGrid(textual).instrument,canonical);
    for (const auto* history : {"canonical","textual"}) {
        const auto raw = root / history;
        {
            capture::Writer writer(config(raw),header(canonical));
            append(writer,Epoch,snapshot()); ticks(writer,1,5); writer.close();
        }
        {
            capture::Writer writer(config(raw),header(std::string_view(history) == "textual" ? textual : canonical));
            append(writer,Epoch + 5 * Minute + 5000,json::object(),capture::Kind::CaptureStarted);
            append(writer,Epoch + 5 * Minute + 6000,snapshot()); ticks(writer,6,20); writer.close();
        }
        RollOptions o{raw,root / (std::string(history) + "-out"),Product,Epoch,Epoch + 20 * Minute};
        const auto report = roll(o);
        EXPECT_EQ(report["days"][0]["instrumentHistory"],json::array({{{"fromMs",Epoch},{"instrument",canonical}}}));
        const auto day = read(o.outputRoot / Product / "roller.json")["days"][std::to_string(Epoch)];
        EXPECT_EQ(day.at("venue"),"kalshi"); EXPECT_EQ(day.at("instrumentHistory"),report["days"][0]["instrumentHistory"]);
        const auto anchor = AnchorStore::load(o.outputRoot / Product / "anchors/2026-10-24/0015.anchor");
        EXPECT_EQ(anchor.instrument,canonical);
        const auto expected = files(o.outputRoot);
        const auto resumed = roll(o);
        ASSERT_TRUE(resumed["days"][0].contains("anchorBoundaryMs")) << resumed;
        EXPECT_EQ(resumed["days"][0]["anchorBoundaryMs"],Epoch + 15 * Minute);
        EXPECT_EQ(files(o.outputRoot),expected);
    }
    EXPECT_EQ(files(root / "canonical-out"),files(root / "textual-out"));
    EXPECT_EQ(read(root / "canonical-out" / Product / "roller.json")["configHash"],
              read(root / "textual-out" / Product / "roller.json")["configHash"]);
}

TEST(KalshiRecovery, NoCheckpointAnchorAcceptsBandsAndRejectsVenue) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    oneRun(root / "raw",header(spec()));
    RollOptions baseline{root / "raw",root / "baseline",Product,Epoch,Epoch + 4 * Minute};
    roll(baseline); const auto expected = files(baseline.outputRoot);
    const auto original = AnchorStore::load(baseline.outputRoot / Product / "anchors/2026-10-24/0000.anchor");
    for (bool coinbase : {false,true}) {
        const auto source = root / (coinbase ? "coinbase" : "different-instrument");
        const json h = coinbase ? json{{"product_metadata",{{"product_id",Product},
            {"quote_increment","0.01"},{"base_increment","0.01"}}}} : header(spec("0.001"));
        oneRun(source / "raw",h,coinbase);
        RollOptions other{source / "raw",source / "out",Product,Epoch,Epoch + 4 * Minute};
        roll(other);
        if (!coinbase) {
            EXPECT_EQ(read(other.outputRoot / Product / "roller.json")["configHash"],
                      read(baseline.outputRoot / Product / "roller.json")["configHash"]);
            EXPECT_EQ(files(other.outputRoot),expected);
        }
        auto offered = AnchorStore::load(other.outputRoot / Product / "anchors/2026-10-24/0000.anchor");
        // Make the durable cursor available in the actual journal. All policy
        // identity checks pass without a checkpoint. Band differences are accepted;
        // a foreign venue is rejected before importing its feed/recorder state.
        offered.pos = original.pos; offered.gridSnapshot = original.gridSnapshot;
        auto state = json::from_cbor(offered.feed.bytes);
        state["feed"]["run"] = original.pos.run;
        offered.feed.bytes = json::to_cbor(state);
        auto o = baseline; o.outputRoot = source / "fallback";
        AnchorStore::write(o.outputRoot,offered);
        ASSERT_FALSE(fs::exists(o.outputRoot / Product / "roller.json"));
        const auto report = roll(o);
        if (coinbase) {
            EXPECT_EQ(report["days"][0]["anchorRejection"],"anchor venue/instrument mismatch") << report;
            EXPECT_FALSE(report["days"][0].contains("anchorBoundaryMs"));
        } else {
            EXPECT_FALSE(report["days"][0].contains("anchorRejection")) << report;
            EXPECT_EQ(report["days"][0]["anchorBoundaryMs"],Epoch);
        }
        EXPECT_EQ(files(o.outputRoot),expected);
    }
}

TEST(KalshiRecovery, ThreeHourDeltasOneSidedMidnightAndHourCrashHaveIdenticalBytes) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto start = Epoch + 23 * Hour, midnight = Epoch + Day, end = midnight + 2 * Hour;
    {
        capture::Writer writer(config(root / "raw"),header(spec()));
        append(writer,start,snapshot());
        bool add = true;
        for (int i = 1; i <= 2161; ++i) {
            const bool openAsk = i == 721; // First delta after the midnight anchor.
            json delta = {{"type","orderbook_delta"},{"sid",1},{"seq",i + 1},
                {"msg",{{"market_ticker",Product},{"price_dollars",openAsk ? "0.4200" : "0.5600"},
                    {"delta_fp",openAsk ? "4.00" : add ? "2.00" : "-2.00"},{"side",openAsk ? "no" : "yes"}}}};
            append(writer,start + i * 5000,delta);
            if (!openAsk) add = !add;
        }
        writer.close();
    }
    RollOptions o{root / "raw",root / "full",Product,start,end};
    roll(o); const auto uninterrupted = files(o.outputRoot);
    ASSERT_TRUE(uninterrupted.contains(Product + "/deep-3600000/2026-10-24.hmc2"));
    ASSERT_TRUE(uninterrupted.contains(Product + "/deep-3600000/2026-10-25.hmc2"));
    const auto hours = recording::Hmc2Store::readRange(o.outputRoot,Product,"deep",Hour,start,end);
    ASSERT_EQ(hours.size(),3u);
    EXPECT_EQ(hours[0].observedMs,Hour);
    bool found = false;
    for (const auto& entry : hours[0].entries) if (entry.row == 56 && !entry.isAsk) {
        // First hour alternates 10 and 12 every five seconds: 1800 seconds
        // at each size, independently of recorder integration or anchor state.
        const double expected = (10. * 1800 + 12. * 1800) / 3600;
        const double tolerance = expected * std::expm1(std::log(2.) / (2 * hours[0].header.sizeScale.codesPerOctave)) + 1e-9;
        EXPECT_NEAR(recording::decodeSize(entry.twapCode,hours[0].header.sizeScale),expected,tolerance);
        found = true;
    }
    EXPECT_TRUE(found);
    const auto anchorFile = o.outputRoot / Product / "anchors/2026-10-25/0000.anchor";
    const auto anchor = AnchorStore::load(anchorFile);
    const auto state = json::from_cbor(anchor.feed.bytes);
    ASSERT_EQ(state["book"]["bids"].size(),1u); ASSERT_TRUE(state["book"]["asks"].empty());
    JournalReader inventory(o.journalRoot,Product);
    fs::create_directories(root / "day2");
    for (const auto& file : inventory.files()) if (file.header.at("opened_system_ns").get<int64_t>() / 1'000'000 >= midnight)
        fs::copy_file(file.path,root / "day2" / file.path.filename());
    auto resumed = o; resumed.journalRoot = root / "day2"; resumed.outputRoot = root / "anchored"; resumed.fromMs = midnight;
    AnchorStore::write(resumed.outputRoot,anchor);
    const auto report = roll(resumed);
    EXPECT_EQ(report["days"][0]["anchorBoundaryMs"],midnight);
    EXPECT_EQ(files(resumed.outputRoot),files(o.outputRoot,"2026-10-25"));
    const auto restoredHours = recording::Hmc2Store::readRange(resumed.outputRoot,Product,"deep",Hour,midnight,end);
    ASSERT_EQ(restoredHours.size(),2u); EXPECT_DOUBLE_EQ(restoredHours.back().midClose,.57);
    auto crash = o; crash.outputRoot = root / "crashed";
    int64_t applied = 0;
    crash.onApplied = [&](const JournalRecord& r) { applied = r.record.time.systemNs / 1'000'000; };
    crash.afterRecordForTest = [&](uint64_t) { if (applied >= midnight + Hour + 5000) throw std::runtime_error("hour crash"); };
    EXPECT_THROW(roll(crash),std::runtime_error);
    crash.onApplied = {}; crash.afterRecordForTest = {};
    const auto recovered = roll(crash);
    EXPECT_TRUE(recovered["days"][1].contains("anchorBoundaryMs"));
    EXPECT_EQ(files(crash.outputRoot),uninterrupted);
}

static void instrumentChanges(bool sizes) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto midnight = Epoch + Day, end = midnight + 30 * Minute;
    const auto old = spec();
    auto fine = sizes ? spec() : spec("0.001"), coarse = sizes ? spec() : spec("0.004");
    if (sizes) { fine["size_increment"] = "1"; coarse["size_increment"] = "0.02"; }
    const json expected = json::array({{{"fromMs",midnight},{"instrument",old}},
        {{"fromMs",midnight + 3000},{"instrument",fine}},
        {{"fromMs",midnight + 5 * Minute + 3000},{"instrument",coarse}},
        {{"fromMs",midnight + 17 * Minute + 3000},{"instrument",old}}});
    for (bool changing : {false,true}) {
        const auto raw = root / (changing ? "mixed-raw" : "constant-raw");
        {
            capture::Writer writer(config(raw),header(old));
            append(writer,midnight - 2 * Minute,snapshot());
            append(writer,midnight - Minute + 3000,json::object(),capture::Kind::CaptureStarted);
            writer.close();
        }
        for (int run = 0; run < 3; ++run) {
            const int first = run == 0 ? 0 : run == 1 ? 5 : 17;
            const int last = run == 0 ? 4 : run == 1 ? 16 : 30;
            capture::Writer writer(config(raw),header(changing ? (run == 0 ? fine : run == 1 ? coarse : old) : old));
            append(writer,midnight + first * Minute + 3000,json::object(),capture::Kind::CaptureStarted);
            append(writer,midnight + first * Minute + 4000,snapshot());
            for (int i = first + 1; i <= last; ++i)
                append(writer,midnight + i * Minute + 3000,json::object(),capture::Kind::CaptureStarted);
            writer.close();
        }
    }
    RollOptions o{root / "mixed-raw",root / "full",Product,midnight - 2 * Minute,end};
    const auto report = roll(o);
    const auto checkpoint = read(o.outputRoot / Product / "roller.json");
    EXPECT_EQ(checkpoint["days"][std::to_string(midnight)]["instrumentHistory"],expected);
    EXPECT_EQ(report["days"][1]["instrumentHistory"],expected);
    EXPECT_EQ(checkpoint["days"][std::to_string(Epoch)]["configHash"],checkpoint["days"][std::to_string(midnight)]["configHash"]);
    const auto same = roll(o); // Completed checkpoint must not truncate its history to an older anchor.
    EXPECT_EQ(same["days"][1]["instrumentHistory"],expected);
    EXPECT_EQ(read(o.outputRoot / Product / "roller.json"),checkpoint);
    auto constant = o; constant.journalRoot = root / "constant-raw"; constant.outputRoot = root / "constant";
    roll(constant);
    EXPECT_EQ(files(o.outputRoot),files(constant.outputRoot));
    EXPECT_EQ(checkpoint["configHash"],read(constant.outputRoot / Product / "roller.json")["configHash"]);
    for (const auto* layer : {"near","deep"}) {
        const auto rows = recording::Hmc2Store::readRange(o.outputRoot,Product,layer,Minute,o.fromMs,end);
        ASSERT_FALSE(rows.empty());
        for (const auto& row : rows) EXPECT_EQ(row.header.configHash,rows.front().header.configHash);
    }
    // Restore a prior-instrument midnight anchor against a changed journal cursor,
    // with no checkpoint and no prior-day journal available.
    const auto anchor = AnchorStore::load(o.outputRoot / Product / "anchors/2026-10-25/0000.anchor");
    EXPECT_EQ(anchor.instrument,old);
    JournalReader inventory(o.journalRoot,Product); fs::create_directories(root / "day2");
    for (const auto& file : inventory.files()) if (file.header.at("opened_system_ns").get<int64_t>() / 1'000'000 >= midnight)
        fs::copy_file(file.path,root / "day2" / file.path.filename());
    auto resumed = o; resumed.fromMs = midnight; resumed.journalRoot = root / "day2"; resumed.outputRoot = root / "anchor-only";
    AnchorStore::write(resumed.outputRoot,anchor);
    const auto restored = roll(resumed);
    EXPECT_EQ(restored["days"][0]["anchorBoundaryMs"],midnight);
    EXPECT_EQ(restored["days"][0]["instrumentHistory"],expected);
    EXPECT_EQ(files(resumed.outputRoot),files(o.outputRoot,"2026-10-25"));
    // Both journal fallback and quarter-anchor recovery cross metadata changes;
    // the persisted list must have exactly the uninterrupted entries.
    for (bool anchors : {false,true}) {
        auto crash = o; crash.outputRoot = root / (anchors ? "crash-anchor" : "crash-journal"); crash.useAnchors = anchors;
        int64_t applied = 0;
        crash.onApplied = [&](const JournalRecord& r) { applied = r.record.time.systemNs / 1'000'000; };
        crash.afterRecordForTest = [&](uint64_t) { if (applied >= midnight + (anchors ? 16 : 19) * Minute) throw std::runtime_error("metadata crash"); };
        EXPECT_THROW(roll(crash),std::runtime_error);
        crash.onApplied = {}; crash.afterRecordForTest = {};
        const auto recovered = roll(crash);
        EXPECT_EQ(recovered["days"][1]["instrumentHistory"],expected);
        EXPECT_EQ(read(crash.outputRoot / Product / "roller.json")["days"][std::to_string(midnight)]["instrumentHistory"],expected);
        EXPECT_EQ(files(crash.outputRoot),files(o.outputRoot));
        if (anchors) EXPECT_EQ(recovered["days"][1]["anchorBoundaryMs"],midnight + 15 * Minute);
    }
}

TEST(KalshiRecovery, BandChangesKeepGridCheckpointAnchorsAndDeterministicHistory) {
    instrumentChanges(false);
}
TEST(KalshiRecovery, SizeChangesKeepGridCheckpointAnchorsAndDeterministicHistory) {
    instrumentChanges(true);
}

TEST(KalshiRecovery, OffGridSnapshotAndDeltaStopBeforeQuantization) {
    for (bool update : {false,true}) for (bool no : {false,true}) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        {
            capture::Writer writer(config(root / "raw"),header(spec()));
            append(writer,Epoch,snapshot()); ticks(writer,1,5); writer.close();
        }
        RollOptions o{root / "raw",root / "out",Product,Epoch,Epoch + 5 * Minute};
        roll(o); const auto prefix = files(o.outputRoot);
        const auto checkpoint = read(o.outputRoot / Product / "roller.json");
        {
            capture::Writer writer(config(root / "raw"),header(spec()));
            // Re-anchor the new run without advancing beyond the saved prefix.
            append(writer,Epoch + 5 * Minute + 3001,snapshot());
            auto bad = snapshot(); bad["seq"] = 2;
            bad["msg"][no ? "no_dollars_fp" : "yes_dollars_fp"] = json::array({{"0.0005","1.00"}});
            if (update) bad = {{"type","orderbook_delta"},{"sid",1},{"seq",2},
                {"msg",{{"market_ticker",Product},{"price_dollars","0.0005"},{"delta_fp","1.00"},{"side",no ? "no" : "yes"}}}};
            append(writer,Epoch + 5 * Minute + 3002,bad); ticks(writer,6,10); writer.close();
        }
        o.toMs = Epoch + 10 * Minute;
        try { roll(o); FAIL() << "quantized an off-grid level"; }
        catch (const std::logic_error& e) {
            EXPECT_EQ(std::string(e.what()),no ? "Kalshi grid: price 0.9995 is finer than 0.001" : "Kalshi grid: price 0.0005 is finer than 0.001");
        }
        EXPECT_EQ(files(o.outputRoot),prefix);
        EXPECT_EQ(read(o.outputRoot / Product / "roller.json"),checkpoint);
    }
}
