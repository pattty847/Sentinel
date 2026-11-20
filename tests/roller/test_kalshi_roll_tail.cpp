// Receive-time tails can contain exchange-time book work for the previous day.
// Journals/output are temporary; the three-file pin comes from 6f2226a5 sources.
#include "roller/Roller.hpp"
#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QDebug>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <cmath>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Minute = 60'000, Day = 86'400'000;
const int64_t Epoch = parseTime("2026-10-24"), Midnight = Epoch + Day, Start = Midnight - 10 * Minute;
const std::string Product = "KXTAIL-TEST";
json spec(const char* step = "0.01") {
    return {{"price_min","0"},{"price_max","1"},{"size_increment","0.01"},
        {"price_ranges",{{{"start","0"},{"end","1"},{"step",step}}}}};
}
capture::WriterConfig config(const fs::path& root) {
    capture::WriterConfig c; c.root = QString::fromStdString(root.string()); c.symbol = Product; c.fsyncBlocks = 0; return c;
}
json header(const json& instrument = spec()) {
    return {{"venue","kalshi"},{"product_metadata",{{"product_id",Product}}},{"instrument",instrument}};
}
json snapshot(const char* price = "0.5600", int seq = 1) {
    return {{"type","orderbook_snapshot"},{"sid",1},{"seq",seq},
        {"msg",{{"market_ticker",Product},{"yes_dollars_fp",json::array({{price,"10.00"}})}}}};
}
json delta(const char* price = "0.5800", int seq = 2) {
    return {{"type","orderbook_delta"},{"sid",1},{"seq",seq},
        {"msg",{{"market_ticker",Product},{"price_dollars",price},{"delta_fp","2.00"},{"side","yes"}}}};
}
void append(capture::Writer& w, int64_t t, const json& j = json::object(), capture::Kind kind = capture::Kind::CaptureStarted) {
    w.append({kind,{t * 1'000'000,(t - Epoch + 1) * 1'000'000},1,j.dump()});
}
void book(capture::Writer& w, int64_t local, const json& j) { append(w,local,j,capture::Kind::Frame); }
void prefix(capture::Writer& w) {
    book(w,Start,snapshot());
    for (auto t = Start + 20'000; t < Midnight; t += 20'000) append(w,t);
}
json read(const fs::path& path) { std::ifstream in(path); return json::parse(in); }
std::map<std::string,std::string> files(const fs::path& root) {
    std::map<std::string,std::string> out;
    if (fs::exists(root)) for (const auto& f : fs::recursive_directory_iterator(root)) if (f.path().extension() == ".hmc2") {
        std::ifstream in(f.path(),std::ios::binary);
        out[fs::relative(f.path(),root).generic_string()] = {std::istreambuf_iterator<char>(in),{}};
    }
    return out;
}
RollOptions options(const fs::path& root) { return {root / "raw",root / "out",Product,Start,Midnight}; }
std::mutex logMutex;
std::vector<std::string>* messages = nullptr;
struct Logs {
    std::vector<std::string> lines;
    QtMessageHandler previous;
    Logs() {
        std::lock_guard lock(logMutex); messages = &lines;
        previous = qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& message) {
            std::lock_guard lock(logMutex); if (messages) messages->push_back(message.toStdString());
        });
    }
    ~Logs() { qInstallMessageHandler(previous); std::lock_guard lock(logMutex); messages = nullptr; }
    bool contains(const std::string& needle) {
        std::lock_guard lock(logMutex);
        return std::any_of(lines.begin(),lines.end(),[&](const auto& line) { return line.find(needle) != std::string::npos; });
    }
};
}

TEST(KalshiTail, LateDeltaMatchesBeforeMidnightAnd6f2226a5Bytes) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    for (bool late : {false,true}) {
        const auto dir = root / (late ? "late" : "early");
        capture::Writer w(config(dir / "raw"),header()); prefix(w);
        auto update = delta(); update["msg"]["ts_ms"] = Midnight - 500;
        book(w,late ? Midnight + 3 : Midnight - 497,update);
        append(w,Midnight + 3000); w.close();
        auto o = options(dir); const auto report = roll(o);
        EXPECT_EQ(report["days"][0]["committedThroughMs"],Midnight);
        const auto rows = recording::Hmc2Store::readRange(o.outputRoot,Product,"near",Minute,Midnight - Minute,Midnight);
        ASSERT_EQ(rows.size(),1u); EXPECT_DOUBLE_EQ(rows[0].midClose,.79);
        bool found = false;
        for (const auto& entry : rows[0].entries) if (entry.row == 580 && !entry.isAsk) {
            const auto expected = 2. * 500 / Minute;
            const auto tolerance = expected * std::expm1(std::log(2.) / (2 * rows[0].header.sizeScale.codesPerOctave)) + 1e-9;
            EXPECT_NEAR(recording::decodeSize(entry.twapCode,rows[0].header.sizeScale),expected,tolerance);
            found = true;
        }
        EXPECT_TRUE(found);
    }
    EXPECT_EQ(files(root / "late/out"),files(root / "early/out"));
    const auto output = files(root / "late/out"); ASSERT_EQ(output.size(),3u);
    json hashes = json::object();
    for (const auto& [name,bytes] : output)
        hashes[name] = QCryptographicHash::hash(QByteArray::fromStdString(bytes),QCryptographicHash::Sha256).toHex().toStdString();
    std::cout << "KALSHI_TAIL_HASHES " << hashes.dump() << '\n';
    EXPECT_EQ(hashes,read(fs::path(KALSHI_FIXTURES) / "kalshi_tail_6f2226a5.json").at("sha256"));
}

TEST(KalshiTail, ExchangeTimeAdmissionMatrixAndHistory) {
    // Same-run frames isolate envelope admission. A changed real file header is
    // produced by another run; snapshots can establish its new delta base.
    for (bool snap : {false,true}) for (const auto* timing : {"before","at","after","missing"})
        for (const auto* policy : {"valid","off-grid","bad-header","changed-header"}) {
        SCOPED_TRACE(snap);
        SCOPED_TRACE(timing);
        SCOPED_TRACE(policy);
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        const bool before = std::string_view(timing) == "before";
        const bool offGrid = std::string_view(policy) == "off-grid";
        const bool badHeader = std::string_view(policy) == "bad-header";
        const bool changedHeader = std::string_view(policy) == "changed-header";
        const bool newRun = badHeader || changedHeader;
        auto event = snap ? snapshot(offGrid ? "0.5805" : "0.5800",2) : delta(offGrid ? "0.5805" : "0.5800");
        if (std::string_view(timing) != "missing") event["msg"]["ts_ms"] = before ? Midnight - 500 :
            std::string_view(timing) == "at" ? Midnight : Midnight + 1;
        {
            capture::Writer w(config(root / "raw"),header()); prefix(w);
            if (!newRun) { book(w,Midnight + 3,event); append(w,Midnight + 3000); }
            w.close();
        }
        if (newRun) {
            capture::Writer w(config(root / "raw"),header(spec(badHeader ? "0.0005" : "0.004")));
            if (!snap) book(w,Midnight + 1,snapshot()); // receive-time envelope; no old-day snapshot
            event["seq"] = snap ? 1 : 2;
            book(w,Midnight + 3,event); append(w,Midnight + 3000); w.close();
        }
        auto o = options(root); json report; ASSERT_NO_THROW(report = roll(o));
        EXPECT_EQ(report["days"][0]["committedThroughMs"],Midnight);
        const auto history = json::array({{{"fromMs",Start},{"instrument",spec()}}});
        EXPECT_EQ(report["days"][0]["instrumentHistory"],history);
        EXPECT_EQ(read(o.outputRoot / Product / "roller.json")["days"][std::to_string(Epoch)]["instrumentHistory"],history);
        const auto rows = recording::Hmc2Store::readRange(o.outputRoot,Product,"near",Minute,Midnight - Minute,Midnight);
        ASSERT_EQ(rows.size(),1u);
        const bool usable = before && !offGrid && !badHeader;
        const bool found = std::any_of(rows[0].entries.begin(),rows[0].entries.end(),[](const auto& e) { return e.row == 580 && !e.isAsk; });
        // A late delta cannot restore a new run's invalidated book; a snapshot can.
        EXPECT_EQ(found,usable && (snap || !newRun));
        const bool rejected = before && (offGrid || badHeader);
        EXPECT_EQ(report["days"][0].value("tailRejectedBatches",0),rejected ? 1 : 0);
        if (rejected) EXPECT_EQ(rows[0].observedMs,40'000);
        const auto bytes = files(o.outputRoot); EXPECT_NO_THROW(roll(o)); EXPECT_EQ(files(o.outputRoot),bytes);
        if (rejected) {
            // Tolerating the closing day's unusable tail never makes it usable
            // on its own day, nor permits a single next-day column to be written.
            o.outputRoot = root / "next-out"; o.fromMs = Midnight; o.toMs = Midnight + Minute;
            try { roll(o); FAIL() << "accepted unusable batch on its own day"; }
            catch (const std::exception& e) {
                EXPECT_EQ(std::string(e.what()),badHeader ?
                    "Kalshi grid: band price/step must be an integer multiple of 0.001" :
                    "Kalshi grid: price 0.5805 is finer than 0.001");
            }
            EXPECT_TRUE(files(o.outputRoot).empty());
        }
    }
}

TEST(KalshiTail, TailFencesOncePerSecondPlusFinalDrain) {
    QTemporaryDir temp; const fs::path root = temp.path().toStdString();
    capture::Writer w(config(root / "raw"),header()); prefix(w);
    auto update = delta(); update["msg"]["ts_ms"] = Midnight - 500;
    book(w,Midnight + 3,update);
    for (int i = 1; i <= 30; ++i) append(w,Midnight + i * 100 + 3);
    w.close();
    auto o = options(root); int64_t local = 0; std::vector<int64_t> fences;
    o.onApplied = [&](const JournalRecord& r) { local = r.record.time.systemNs / 1'000'000; };
    o.afterFenceForTest = [&] { if (local >= Midnight) fences.push_back(local); };
    EXPECT_EQ(roll(o)["days"][0]["committedThroughMs"],Midnight);
    EXPECT_EQ(fences,(std::vector<int64_t>{Midnight + 3,Midnight + 1003,Midnight + 2003,Midnight + 3003,Midnight + 3003}));
}

TEST(KalshiTail, RefusalSurvivesFenceFailureAndReportsIt) {
    for (bool badHeader : {false,true}) for (bool unknown : {false,true}) {
        QTemporaryDir temp; const fs::path root = temp.path().toStdString();
        {
            capture::Writer w(config(root / "raw"),header()); book(w,Epoch,snapshot());
            for (int i = 1; i <= 17; ++i) append(w,Epoch + i * 20'000);
            if (!badHeader) book(w,Epoch + 350'000,delta("0.5805"));
            w.close();
        }
        if (badHeader) {
            capture::Writer w(config(root / "raw"),header(spec("0.0005"))); append(w,Epoch + 350'000); w.close();
        }
        auto o = options(root); o.fromMs = Epoch; o.toMs = Epoch + 10 * Minute;
        o.onCommitted = [&](int64_t through) {
            if (through == Epoch + 5 * Minute) {
                if (unknown) throw 7;
                throw std::runtime_error("injected fence failure");
            }
        };
        Logs logs;
        try { roll(o); FAIL() << "accepted refusal"; }
        catch (const std::exception& e) {
            EXPECT_EQ(std::string(e.what()),badHeader ?
                "Kalshi grid: band price/step must be an integer multiple of 0.001" :
                "Kalshi grid: price 0.5805 is finer than 0.001");
        } catch (...) { FAIL() << "lost refusal to non-standard fence exception"; }
        EXPECT_TRUE(logs.contains("Kalshi refusal checkpoint fence failed"));
        EXPECT_TRUE(logs.contains(unknown ? "unknown exception" : "injected fence failure"));
    }
}
