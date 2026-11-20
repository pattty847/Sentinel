// Offline acceptance: independent Python book snapshots supply the TWAP oracle;
// no reference decoder, production data, network, or live services are used.
#include "roller/Roller.hpp"
#include "roller/AnchorStore.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <fstream>
#include <cmath>
#include <iostream>
#include <numeric>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Minute = 60'000, Day = 86'400'000;
const int64_t Epoch = parseTime("2026-10-24");
const std::string A = "KXATPMATCH-26OCT24COBMUS-MUS";
const std::string B = "KXSUPERLIGGAME-26OCT24SAMAMS-AMS";
json read(const fs::path& path) { std::ifstream in(path); return json::parse(in); }
json instrument(const std::string& step = "0.01") {
    return {{"price_min", "0"}, {"price_max", "1"}, {"size_increment", "0.01"},
        {"price_ranges", json::array({{{"start", "0"}, {"end", "1"}, {"step", step}}})}};
}
json header(const std::string& product, const json& spec) {
    return {{"venue", "kalshi"}, {"product_metadata", {{"product_id", product}}}, {"instrument", spec}};
}
json snapshot(const std::string& product, bool empty = false) {
    return {{"type", "orderbook_snapshot"}, {"sid", 1}, {"seq", 1},
        {"msg", {{"market_ticker", product}, {"yes_dollars_fp", empty ? json::array() : json::array({{"0.5600", "10.25"}})}}}};
}
void append(capture::Writer& writer, int64_t t, const json& frame,
            capture::Kind kind = capture::Kind::Frame) {
    writer.append({kind, {t * 1'000'000, (t - Epoch + 1) * 1'000'000}, 1, frame.dump()});
}
void simple(const fs::path& root, const std::string& product, const json& spec,
            bool empty = false, int minutes = 4) {
    capture::WriterConfig cfg; cfg.root = QString::fromStdString(root.string()); cfg.symbol = product; cfg.fsyncBlocks = 0;
    capture::Writer writer(cfg, header(product, spec));
    append(writer, Epoch, snapshot(product, empty));
    for (int i = 1; i <= minutes; ++i)
        append(writer, Epoch + i * Minute + 3000, json::object(), capture::Kind::CaptureStarted);
    writer.close();
}
std::map<std::string, std::string> hmcFiles(const fs::path& root, const std::string& day = "") {
    std::map<std::string, std::string> out;
    if (!fs::exists(root)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().extension() != ".hmc2" || (!day.empty() && !entry.path().filename().string().starts_with(day))) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        out[fs::relative(entry.path(), root).generic_string()] = {std::istreambuf_iterator<char>(in), {}};
    }
    return out;
}
int64_t atoms(std::string text) { text.erase(text.find('.'), 1); return std::stoll(text); }
// Independent integration from the 43 expected books. Receipt ticks use the
// last local-envelope offset; both event time and ticks are monotone-clamped.
struct Oracle {
    std::map<std::pair<int64_t, bool>, long double> sums;
    json book;
    int64_t clock = 0, offset = 0, observed = 0, end;
    double open = 0, close = 0, low = 1, high = 0;
    explicit Oracle(int64_t limit) : end(limit) {}
    void advance(int64_t t) {
        t = std::max(clock, std::min(t, end));
        if (!book.is_null()) {
            const auto dt = t - clock;
            observed += dt;
            for (bool ask : {false, true}) for (const auto& row : book.at(ask ? "no_bids" : "yes_bids")) {
                const auto p = atoms(row[0]);
                const auto price = ask ? 10000 - p : p;
                sums[{price / 100, ask}] += static_cast<long double>(atoms(row[1])) / 100 * dt;
            }
        }
        clock = t;
    }
    void change(const json& next) {
        const bool first = book.is_null();
        book = next;
        int64_t bid = 0, no = 0;
        for (const auto& row : book.at("yes_bids")) bid = std::max(bid, atoms(row[0]));
        for (const auto& row : book.at("no_bids")) no = std::max(no, atoms(row[0]));
        close = double(bid + (10000 - no)) / 20000;
        if (first) open = close;
        low = std::min(low, close); high = std::max(high, close);
    }
};
}

TEST(KalshiRoll, GridTableAndMalformedPoliciesWriteNothing) {
    auto tapered = instrument("0.001");
    tapered["price_ranges"] = {{{"start", "0"}, {"end", "0.1"}, {"step", "0.001"}},
        {{"start", "0.1"}, {"end", "0.9"}, {"step", "0.01"}},
        {{"start", "0.9"}, {"end", "1"}, {"step", "0.001"}}};
    auto mixed = instrument("0.005");
    mixed["price_ranges"] = {{{"start", "0"}, {"end", "0.5"}, {"step", "0.005"}},
        {{"start", "0.5"}, {"end", "1"}, {"step", "0.01"}}};
    struct Case { json spec; double scale, near; int64_t rows; };
    for (const auto& c : std::vector<Case>{{instrument(), 100, .01, 100},
             {instrument("0.001"), 1000, .001, 1000}, {tapered, 1000, .001, 1000}, {mixed, 1000, .005, 200}}) {
        const auto g = deriveKalshiGrid(c.spec);
        EXPECT_EQ(g.priceScale, c.scale); EXPECT_EQ(g.nearTick, c.near); EXPECT_EQ(g.deepTick, .01);
        const auto cfg = g.config("unused");
        ASSERT_TRUE(cfg.priceBounds); EXPECT_EQ(cfg.priceBounds->lower, 0); EXPECT_EQ(cfg.priceBounds->upper, 1);
        ASSERT_EQ(cfg.layers.size(), 2u);
        ASSERT_TRUE(cfg.layers[0].absoluteRows); EXPECT_EQ(cfg.layers[0].absoluteRows->low, 0);
        EXPECT_EQ(cfg.layers[0].absoluteRows->high, c.rows);
        EXPECT_EQ(cfg.layers[1].absoluteRows->high, 100);
        EXPECT_EQ(cfg.sizeScale.floor, .01); EXPECT_FALSE(cfg.layers[0].hourlyRollup); EXPECT_TRUE(cfg.layers[1].hourlyRollup);
    }
    std::vector<json> malformed;
    auto bad = mixed; bad["price_ranges"][1]["step"] = "0.007"; malformed.push_back(bad);
    bad = instrument("0.003"); malformed.push_back(bad);
    bad = mixed; bad["price_ranges"][1]["start"] = "0.51"; malformed.push_back(bad);
    bad = mixed; bad["price_ranges"][0]["end"] = "0.501"; bad["price_ranges"][1]["start"] = "0.501"; malformed.push_back(bad);
    bad = instrument(); bad["price_ranges"][0]["end"] = "0.99"; malformed.push_back(bad);
    bad = instrument(); bad["price_min"] = "0.01"; bad["price_ranges"][0]["start"] = "0.01"; malformed.push_back(bad);
    for (const auto& spec : malformed) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        simple(root / "raw", A, spec);
        RollOptions o{root / "raw", root / "out", A, Epoch, Epoch + Minute};
        try { roll(o); FAIL() << "accepted " << spec; }
        catch (const std::exception& e) { EXPECT_NE(std::string(e.what()).find("Kalshi grid:"), std::string::npos) << e.what(); }
        EXPECT_FALSE(fs::exists(o.outputRoot));
    }
}

TEST(KalshiRoll, OneSidedAndEmptyStartUseBoundsIncludingExtendedTickers) {
    for (bool empty : {false, true}) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        const std::string product = "KXTEST-1.5-" + std::string(35, 'A');
        simple(root / "raw", product, instrument(), empty);
        RollOptions o{root / "raw", root / "out", product, Epoch, Epoch + 4 * Minute};
        const auto report = roll(o);
        EXPECT_EQ(report["days"][0]["committedThroughMs"], o.toMs);
        for (const auto* layer : {"near", "deep"}) {
            const auto rows = recording::Hmc2Store::readRange(o.outputRoot, product, layer, Minute, o.fromMs, o.toMs);
            ASSERT_EQ(rows.size(), 4u);
            for (const auto& row : rows) {
                EXPECT_EQ(row.observedMs, Minute); EXPECT_DOUBLE_EQ(row.midOpen, empty ? .5 : .78);
                EXPECT_EQ(row.bidRowLo, 0); EXPECT_EQ(row.askRowLo, 0);
                EXPECT_EQ(row.bidRowHi, 100); EXPECT_EQ(row.askRowHi, 100);
                EXPECT_EQ(row.entries.empty(), empty);
            }
        }
        const auto anchors = AnchorStore::candidates(o.outputRoot, product, Epoch);
        ASSERT_FALSE(anchors.empty());
        const auto a = AnchorStore::load(anchors.back());
        EXPECT_FALSE(a.referenceMid); EXPECT_EQ(a.instrument, instrument());
        EXPECT_EQ(a.identity.venue, Venue::Kalshi);
    }
}

TEST(KalshiRoll, RealCaptureBothTickersIndependentWeightedRowsAndMidOhlc) {
    const auto frames = read(fs::path(KALSHI_FIXTURES) / "ws_capture_sample.json");
    const auto steps = read(fs::path(KALSHI_FIXTURES) / "ws_capture_expected_books.json").at("steps");
    for (const auto& product : {A, B}) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        const auto from = frames[0].at("sending_ts_ms").get<int64_t>() / Minute * Minute, end = from + Minute;
        capture::WriterConfig cfg; cfg.root = QString::fromStdString((root / "raw").string()); cfg.symbol = product; cfg.fsyncBlocks = 0;
        capture::Writer writer(cfg, header(product, instrument()));
        Oracle oracle(end); size_t step = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            const auto& frame = frames[i];
            const int64_t local = frame.at("sending_ts_ms");
            append(writer, local, frame);
            const auto type = frame.at("type").get<std::string>();
            const bool ours = (type == "orderbook_snapshot" || type == "orderbook_delta") && frame.at("msg").at("market_ticker") == product;
            if (ours) {
                const int64_t envelope = frame.at("msg").value("ts_ms", local);
                oracle.advance(envelope); oracle.offset = local - envelope;
            }
            if (step < steps.size() && steps[step].at("message_index") == i) {
                if (ours) oracle.change(steps[step].at("books").at(product));
                ++step;
            }
            oracle.advance(local - oracle.offset);
        }
        ASSERT_EQ(step, 43u);
        append(writer, end + 10'000, json::object(), capture::Kind::CaptureStarted); writer.close();
        oracle.advance(end);
        RollOptions o{root / "raw", root / "out", product, from, end};
        const auto report = roll(o);
        EXPECT_EQ(report["days"][0]["committedThroughMs"], end);
        EXPECT_EQ(hmcFiles(o.outputRoot).size(), 2u);
        std::cout << "KALSHI_FIXTURE_REPORT " << report.dump() << '\n';
        for (const auto& [name, bytes] : hmcFiles(o.outputRoot)) std::cout << "KALSHI_HMC2 " << name << '\n';
        for (const auto* layer : {"near", "deep"}) {
            const auto rows = recording::Hmc2Store::readRange(o.outputRoot, product, layer, Minute, from, end);
            ASSERT_EQ(rows.size(), 1u); const auto& row = rows[0];
            EXPECT_EQ(row.header.priceScale, 100); EXPECT_EQ(row.header.rowTickUnits, 1);
            EXPECT_EQ(row.bidRowLo, 0); EXPECT_EQ(row.bidRowHi, 100); EXPECT_EQ(row.askRowLo, 0); EXPECT_EQ(row.askRowHi, 100);
            EXPECT_EQ(row.observedMs, oracle.observed);
            EXPECT_DOUBLE_EQ(row.midOpen, oracle.open); EXPECT_DOUBLE_EQ(row.midClose, oracle.close);
            EXPECT_DOUBLE_EQ(row.midMin, oracle.low); EXPECT_DOUBLE_EQ(row.midMax, oracle.high);
            std::map<std::pair<int64_t, bool>, double> actual;
            for (const auto& e : row.entries) actual[{e.row, e.isAsk}] = recording::decodeSize(e.twapCode, row.header.sizeScale);
            for (int64_t price = 0; price <= 100; ++price) for (bool ask : {false, true}) {
                const auto key = std::pair(price, ask);
                const auto expected = double(oracle.sums[key] / oracle.observed);
                // Nearest logarithmic code: at most half a code step, plus FP noise.
                const auto tolerance = expected * std::expm1(std::log(2.) / (2 * row.header.sizeScale.codesPerOctave)) + 1e-9;
                EXPECT_NEAR(actual[key], expected, tolerance) << product << ' ' << layer << ' ' << price << ' ' << ask;
            }
        }
        fs::create_directories(root / "config");
        std::ofstream(root / "config/server_config.yaml") << "recording:\n  dir: '" << (root / "excluded").string() << "'\n";
        struct Restore { fs::path cwd = fs::current_path(); ~Restore() { fs::current_path(cwd); } } restore;
        fs::current_path(root);
        for (bool dry : {true, false}) {
            std::vector<std::string> args = {"sentinel-roll", (root / "raw").string(), (root / "cli").string(),
                "--products", product, "--from", "2026-10-24T16:40:00Z", "--to", "2026-10-24T16:41:00Z"};
            if (dry) args.push_back("--dry-run");
            std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data());
            testing::internal::CaptureStdout(); const auto code = rollMain(int(argv.size()), argv.data());
            const auto output = testing::internal::GetCapturedStdout();
            ASSERT_EQ(code, 0) << output;
            EXPECT_EQ(json::parse(output)[0]["days"][0]["grid"], report["days"][0]["grid"]);
            if (dry) EXPECT_FALSE(fs::exists(root / "cli"));
            else EXPECT_EQ(hmcFiles(root / "cli"), hmcFiles(o.outputRoot));
        }
    }
}

TEST(KalshiRoll, CrashResumeMatchesUninterrupted) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    simple(root / "raw", A, instrument("0.001"), false, 20);
    RollOptions o{root / "raw", root / "full", A, Epoch, Epoch + 20 * Minute};
    roll(o); const auto expected = hmcFiles(o.outputRoot);
    o.outputRoot = root / "resumed";
    o.afterRecordForTest = [](uint64_t n) { if (n == 18) throw std::runtime_error("crash seam"); };
    EXPECT_THROW(roll(o), std::runtime_error);
    ASSERT_TRUE(fs::exists(o.outputRoot / A / "roller.json"));
    o.afterRecordForTest = {}; const auto report = roll(o);
    EXPECT_TRUE(report["days"][0].contains("anchorBoundaryMs"));
    EXPECT_EQ(hmcFiles(o.outputRoot), expected);
}

TEST(KalshiRoll, BandIdentityRejectsCheckpointAndVenueMismatch) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    auto other = instrument();
    other["price_ranges"] = {{{"start", "0"}, {"end", "0.5"}, {"step", "0.01"}},
        {{"start", "0.5"}, {"end", "1"}, {"step", "0.02"}}};
    simple(root / "raw", A, instrument()); simple(root / "other", A, other);
    RollOptions o{root / "raw", root / "out", A, Epoch, Epoch + 4 * Minute};
    roll(o);
    const auto first = read(o.outputRoot / A / "roller.json").at("configHash");
    EXPECT_EQ(first, 15407106172843635619ull); // Exact policy JSON/FNV pin, bounds and instrument included.
    const auto bytes = hmcFiles(o.outputRoot);
    auto otherOptions = o; otherOptions.journalRoot = root / "other"; otherOptions.outputRoot = root / "other-out";
    roll(otherOptions);
    const auto otherHash = read(otherOptions.outputRoot / A / "roller.json").at("configHash");
    EXPECT_NE(otherHash, first);
    auto checkpoint = read(o.outputRoot / A / "roller.json");
    checkpoint["configHash"] = otherHash;
    checkpoint["days"][std::to_string(Epoch)]["configHash"] = otherHash;
    writeCheckpoint(o.outputRoot / A / "roller.json", checkpoint);
    try { roll(o); FAIL() << "accepted changed policy"; }
    catch (const std::logic_error& e) { EXPECT_NE(std::string(e.what()).find("checkpoint policy/range mismatch"), std::string::npos); }
    EXPECT_EQ(hmcFiles(o.outputRoot), bytes);
    o = otherOptions;
    const auto path = AnchorStore::candidates(o.outputRoot, A, Epoch).back();
    auto a = AnchorStore::load(path);
    const auto encoded = AnchorStore::encode(a);
    EXPECT_EQ(encoded.at(8), 2); // Old readers require envelope version 1.
    EXPECT_EQ(AnchorStore::encode(AnchorStore::decode(encoded)), encoded);
    auto wrong = a.identity; wrong.venue = Venue::Coinbase;
    EXPECT_FALSE(AnchorStore::read(path, wrong, Epoch));
    a.identity.venue = Venue::Coinbase; a.instrument = nullptr; a.referenceMid = 100;
    a.metadata = {{"product_id", A}, {"quote_increment", "0.01"}, {"base_increment", "0.01"}};
    AnchorStore::write(root / "coinbase", a);
    wrong = a.identity; wrong.venue = Venue::Kalshi;
    EXPECT_FALSE(AnchorStore::read(AnchorStore::path(root / "coinbase", a), wrong, Epoch));
    a.referenceMid.reset(); EXPECT_THROW(AnchorStore::encode(a), std::runtime_error);
}

TEST(KalshiRoll, MidnightAnchorAndOldJournalSnapshotHaveIdenticalNextDayBytes) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    const auto midnight = Epoch + Day;
    capture::WriterConfig cfg; cfg.root = QString::fromStdString((root / "raw").string()); cfg.symbol = A; cfg.fsyncBlocks = 0;
    capture::Writer writer(cfg, header(A, instrument("0.001")));
    append(writer, Epoch + 12 * 3'600'000, snapshot(A));
    for (int i = -2; i <= 20; ++i)
        append(writer, midnight + i * Minute + 3000, json::object(), capture::Kind::CaptureStarted);
    writer.close();
    RollOptions o{root / "raw", root / "full", A, midnight - 2 * Minute, midnight + 20 * Minute};
    roll(o); const auto expected = hmcFiles(o.outputRoot, "2026-10-25"); ASSERT_FALSE(expected.empty());
    const auto midnightFile = o.outputRoot / A / "anchors/2026-10-25/0000.anchor";
    ASSERT_TRUE(fs::exists(midnightFile));
    const auto a = AnchorStore::load(midnightFile);
    EXPECT_EQ(json::from_cbor(a.feed.bytes).at("feed").at("version"), 2);
    EXPECT_EQ(json::from_cbor(a.feed.bytes).at("feed").at("venue"), "kalshi");
    // Retain only day 2 journal files: the original snapshot is deliberately absent.
    JournalReader inventory(root / "raw", A);
    fs::create_directories(root / "day2");
    for (const auto& file : inventory.files()) if (file.header.at("opened_system_ns").get<int64_t>() / 1'000'000 >= midnight)
        fs::copy_file(file.path, root / "day2" / file.path.filename());
    o.fromMs = midnight; o.outputRoot = root / "anchored"; o.journalRoot = root / "day2";
    const auto target = o.outputRoot / A / "anchors/2026-10-25/0000.anchor";
    fs::create_directories(target.parent_path()); fs::copy_file(midnightFile, target);
    const auto anchored = roll(o);
    ASSERT_TRUE(anchored["days"][0].contains("anchorBoundaryMs")) << anchored;
    EXPECT_EQ(anchored["days"][0]["anchorBoundaryMs"], midnight);
    EXPECT_EQ(hmcFiles(o.outputRoot), expected);
    o.outputRoot = root / "fallback"; o.journalRoot = root / "raw"; o.useAnchors = o.writeAnchors = false;
    EXPECT_TRUE(inventory.anchor(midnight));
    roll(o); EXPECT_EQ(hmcFiles(o.outputRoot), expected);
}

TEST(KalshiRoll, TickChangeBetweenDaysChangesGridAndHmcPolicy) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    for (int d = 0; d < 2; ++d) {
        capture::WriterConfig cfg; cfg.root = QString::fromStdString((root / "raw").string()); cfg.symbol = A; cfg.fsyncBlocks = 0;
        auto h = header(A, instrument(d ? "0.001" : "0.01"));
        capture::Writer writer(cfg, h);
        append(writer, Epoch + d * Day, snapshot(A));
        append(writer, Epoch + d * Day + Minute + 3000, json::object(), capture::Kind::CaptureStarted);
        writer.close();
    }
    RollOptions o{root / "raw", root / "out", A, Epoch, Epoch + Day + Minute};
    const auto report = roll(o);
    EXPECT_EQ(report["days"][0]["grid"]["nearTick"], .01);
    EXPECT_EQ(report["days"][1]["grid"]["nearTick"], .001);
    const auto cp = read(o.outputRoot / A / "roller.json");
    EXPECT_NE(cp["days"][std::to_string(Epoch)]["configHash"], cp["days"][std::to_string(Epoch + Day)]["configHash"]);
    for (const auto* layer : {"near", "deep"}) {
        const auto before = recording::Hmc2Store::readRange(o.outputRoot, A, layer, Minute, Epoch, Epoch + Minute);
        const auto after = recording::Hmc2Store::readRange(o.outputRoot, A, layer, Minute, Epoch + Day, Epoch + Day + Minute);
        ASSERT_EQ(before.size(), 1u); ASSERT_EQ(after.size(), 1u);
        EXPECT_NE(before[0].header.configHash, after[0].header.configHash);
        EXPECT_EQ(after[0].header.priceScale, 1000);
    }
}

TEST(KalshiRoll, CliDryAndRealReports) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    simple(root / "raw", A, instrument());
    fs::create_directories(root / "config");
    std::ofstream(root / "config/server_config.yaml") << "recording:\n  dir: '" << (root / "live-excluded").string() << "'\n";
    struct RestoreCwd { fs::path old = fs::current_path(); ~RestoreCwd() { fs::current_path(old); } } restore;
    fs::current_path(root);
    for (bool dry : {true, false}) {
        std::vector<std::string> args = {"sentinel-roll", (root / "raw").string(), (root / "out").string(),
            "--products", A, "--from", "2026-10-24", "--to", "2026-10-24T00:04:00Z"};
        if (dry) args.push_back("--dry-run");
        std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data());
        testing::internal::CaptureStdout(); const auto code = rollMain(int(argv.size()), argv.data());
        const auto stdoutText = testing::internal::GetCapturedStdout();
        ASSERT_EQ(code, 0) << stdoutText;
        const auto report = json::parse(stdoutText).at(0);
        EXPECT_EQ(report.at("dryRun"), dry);
        EXPECT_EQ(report["days"][0]["grid"]["venue"], "kalshi");
        EXPECT_EQ(report["days"][0]["grid"]["priceBounds"], json::array({0, 1}));
        EXPECT_EQ(report["days"][0]["grid"]["nearRows"], json::array({0, 100}));
        if (dry) EXPECT_FALSE(fs::exists(root / "out"));
        else EXPECT_EQ(hmcFiles(root / "out").size(), 2u);
    }
}

TEST(KalshiRoll, JournalAnchorSearchAcrossHoursRunsAndForeignSnapshots) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    for (int run = 0; run < 2; ++run) {
        capture::WriterConfig cfg; cfg.root = temp.path(); cfg.symbol = A; cfg.fsyncBlocks = 0;
        auto h = header(A, instrument());
        capture::Writer writer(cfg, h);
        if (!run) {
            append(writer, Epoch + Minute, snapshot(A));
            append(writer, Epoch + 4 * 3'600'000, snapshot(A));
            append(writer, Epoch + 8 * 3'600'000, snapshot(B));
        } else {
            append(writer, Epoch + Day, json::object(), capture::Kind::CaptureStarted);
            append(writer, Epoch + Day + 3'600'000, snapshot(A));
        }
        writer.close();
    }
    JournalReader reader(root, A);
    EXPECT_FALSE(reader.anchor(Epoch));
    const auto old = reader.anchor(Epoch + Day);
    ASSERT_TRUE(old); EXPECT_EQ(old->run, reader.files().front().header.at("run_id").get<std::string>());
    JournalReader selected(root, A, old); JournalRecord record;
    ASSERT_TRUE(selected.next(record)); EXPECT_EQ(record.record.time.systemNs / 1'000'000, Epoch + 4 * 3'600'000);
    const auto newer = reader.anchor(Epoch + Day + 3'600'000);
    ASSERT_TRUE(newer); EXPECT_EQ(newer->run, reader.files().back().header.at("run_id").get<std::string>());
}
