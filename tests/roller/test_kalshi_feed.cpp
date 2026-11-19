// Offline RAWL2 -> JournalReader -> JournalFeed acceptance. The two capture JSON
// files are copied unchanged from the Python reference; all 43 oracle steps are
// checked for both tickers. Synthetic failures never touch production data.
#include "roller/JournalFeed.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <bit>
#include <fstream>
#include <set>

using namespace sentinel;
using namespace sentinel::roller;
using nlohmann::json;
namespace {
constexpr int64_t Epoch = 1'792'860'100'000;
const std::string A = "KXATPMATCH-26OCT24COBMUS-MUS";
const std::string B = "KXSUPERLIGGAME-26OCT24SAMAMS-AMS";
json fixture(const char* name) {
    std::ifstream in(std::filesystem::path(KALSHI_FIXTURES) / name);
    return json::parse(in);
}
json metadata(const std::string& product) {
    return {{"venue", "kalshi"}, {"product_metadata", {{"product_id", product}}},
        {"instrument", {{"price_min", "0.0000"}, {"price_max", "1.0000"}, {"size_increment", "0.01"},
            {"price_ranges", json::array({{{"start", "0.0000"}, {"end", "1.0000"}, {"step", "0.0001"}}})}}}};
}
std::vector<JournalRecord> journal(const json& frames, const std::string& product = A, json header = nullptr) {
    QTemporaryDir temp;
    if (!temp.isValid()) throw std::runtime_error("test temporary directory");
    capture::WriterConfig cfg; cfg.root = temp.path(); cfg.symbol = product; cfg.fsyncBlocks = 0;
    capture::Writer writer(cfg, header.is_null() ? metadata(product) : header);
    size_t index = 0;
    for (const auto& frame : frames) {
        writer.append({capture::Kind::Frame, {(Epoch + int64_t(index)) * 1'000'000, int64_t(index)},
                       1, frame.is_string() ? frame.get<std::string>() : frame.dump()});
        ++index;
    }
    writer.close();
    JournalReader reader(temp.path().toStdString(), product);
    std::vector<JournalRecord> out;
    JournalRecord r;
    while (reader.next(r)) out.push_back(r);
    return out;
}
json snapshot(int seq = 1, const std::string& product = A, int sid = 1) {
    return {{"type", "orderbook_snapshot"}, {"sid", sid}, {"seq", seq},
        {"msg", {{"market_ticker", product}, {"yes_dollars_fp", json::array({json::array({"0.5600", "10.25"})})},
                 {"no_dollars_fp", json::array({json::array({"0.4300", "3.00"})})}}}};
}
json delta(int seq, const char* change = "1.25", const std::string& product = A, int sid = 1) {
    return {{"type", "orderbook_delta"}, {"sid", sid}, {"seq", seq},
        {"msg", {{"market_ticker", product}, {"price_dollars", "0.5600"},
                 {"delta_fp", change}, {"side", "yes"}}}};
}
struct Observer {
    JournalBook book;
    json events = json::array();
    std::vector<std::string> invalid;
    std::vector<Trade> trades;
    size_t snapshots = 0, updates = 0, ticks = 0, boundaries = 0;
    void attach(JournalFeed& feed) {
        const auto levels = [](const auto& rows) {
            json j = json::array();
            for (const auto& l : rows) j.push_back({l.isBid, l.price, l.size});
            return j;
        };
        feed.onSnapshot = [&, levels](auto t, auto local, auto rows) {
            book.snapshot(t, rows); ++snapshots; events.push_back({"snapshot", t, local, levels(rows)});
        };
        feed.onUpdates = [&, levels](auto t, auto local, auto rows) {
            book.apply(t, rows); ++updates; events.push_back({"update", t, local, levels(rows)});
        };
        feed.onInvalid = [&](auto t, const auto& reason) {
            book.clear(); events.push_back({"invalid", t, reason});
            if (reason == "journal boundary") ++boundaries; else invalid.push_back(reason);
        };
        feed.onTrade = [&](const Trade& t) {
            trades.push_back(t);
            events.push_back({"trade", t.trade_id, int(t.side), t.price, t.size,
                std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count()});
        };
        feed.onTick = [&](auto t) { ++ticks; events.push_back({"tick", t}); };
        feed.onConnection = [&](bool up) { events.push_back({"connection", up}); };
        feed.onConnectionChange = [&] { events.push_back({"connectionChange"}); };
    }
};
// Independent oracle conversion, using decimal strings rather than decoder helpers.
int64_t atoms(std::string value) {
    value.erase(value.find('.'), 1);
    return std::stoll(value);
}
uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
void compareBook(const JournalBook& actual, const json& expected) {
    for (const auto* side : {"yes_bids", "no_bids"}) {
        std::map<double, double> want;
        for (const auto& row : expected.at(side)) {
            auto p = atoms(row[0].get<std::string>());
            if (std::string_view(side) == "no_bids") p = 10000 - p;
            want.emplace(double(p) / 10000, double(atoms(row[1].get<std::string>())) / 100);
        }
        const auto& got = std::string_view(side) == "yes_bids" ? actual.bids : actual.asks;
        ASSERT_EQ(got.size(), want.size());
        auto it = got.begin();
        for (const auto& [p, q] : want) {
            EXPECT_EQ(bits(it->first), bits(p)); EXPECT_EQ(bits(it->second), bits(q)); ++it;
        }
    }
}
}
TEST(KalshiFeed, RealCaptureEveryBookSequenceStepAndTrade) {
    const auto frames = fixture("ws_capture_sample.json");
    const auto steps = fixture("ws_capture_expected_books.json").at("steps");
    ASSERT_EQ(frames.size(), 75u); ASSERT_EQ(steps.size(), 43u);
    for (const auto& product : {A, B}) {
        const auto records = journal(frames, product);
        ASSERT_EQ(records.size(), frames.size());
        JournalFeed feed(product); Observer seen; seen.attach(feed);
        size_t step = 0, trades = 0;
        for (size_t i = 0; i < records.size(); ++i) {
            SCOPED_TRACE(product + " message " + std::to_string(i));
            feed.apply(records[i]);
            if (step < steps.size() && steps[step].at("message_index") == i) {
                const auto& books = steps[step].at("books");
                if (books.contains(product)) {
                    EXPECT_TRUE(seen.book.valid); compareBook(seen.book, books.at(product));
                } else { EXPECT_FALSE(seen.book.valid); EXPECT_TRUE(seen.book.bids.empty()); }
                ++step;
            }
            const auto& frame = frames[i];
            if (frame.at("type") == "trade" && frame.at("msg").at("market_ticker") == product) {
                const auto& msg = frame.at("msg");
                ASSERT_EQ(seen.trades.size(), ++trades);
                const auto& t = seen.trades.back();
                EXPECT_EQ(t.product_id, product); EXPECT_EQ(t.trade_id, msg.at("trade_id").get<std::string>());
                EXPECT_EQ(t.side, msg.at("taker_outcome_side") == "yes" ? AggressorSide::Buy : AggressorSide::Sell);
                EXPECT_EQ(bits(t.price), bits(double(atoms(msg.at("yes_price_dollars"))) / 10000));
                EXPECT_EQ(bits(t.size), bits(double(atoms(msg.at("count_fp"))) / 100));
                EXPECT_EQ(t.timestamp.time_since_epoch(), std::chrono::milliseconds(msg.at("ts_ms").get<int64_t>()));
            }
        }
        EXPECT_EQ(step, 43u); EXPECT_EQ(seen.ticks, 75u); EXPECT_EQ(seen.boundaries, 1u);
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_EQ(trades, product == A ? 10u : 0u);
    }
}
TEST(KalshiFeed, DroppedOkDeltaDuplicateAndReorderInvalidateUntilSnapshot) {
    const auto capture = fixture("ws_capture_sample.json");
    for (int mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        auto frames = capture;
        if (mode == 0) frames.erase(frames.begin() + 11); // seq-6 ok
        if (mode == 1) frames.erase(frames.begin() + 7); // seq-4 delta
        if (mode == 2) frames.insert(frames.begin() + 8, capture[7]);
        if (mode == 3) std::swap(frames[6], frames[7]);
        frames.push_back(snapshot(44)); frames.push_back(delta(45));
        auto records = journal(frames);
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        bool broken = false; size_t acceptedAtFailure = 0;
        for (size_t i = 0; i < records.size(); ++i) {
            feed.apply(records[i]);
            if (!broken && !seen.invalid.empty()) { broken = true; acceptedAtFailure = seen.updates; }
            if (broken && i < records.size() - 2) {
                EXPECT_EQ(seen.updates, acceptedAtFailure); EXPECT_FALSE(seen.book.valid);
            }
        }
        EXPECT_TRUE(broken); EXPECT_TRUE(seen.book.valid);
        EXPECT_EQ(seen.updates, acceptedAtFailure + 1);
        EXPECT_EQ(seen.book.bids.at(0.56), 11.5);
    }
}
TEST(KalshiFeed, MalformedBookFramesInvalidateWithoutPartialCallbacks) {
    std::vector<json> bad;
    bad.push_back(delta(2, "-10.26"));
    for (const auto* p : {"0.56001", "1.0001", "-0.01", "abc", "0.56", "+0.5600", " 0.5600"}) {
        auto d = delta(2); d["msg"]["price_dollars"] = p; bad.push_back(d);
    }
    for (const json& q : {json("1.001"), json("nan"), json("1"), json(1.25), json("92233720368547758.08")}) {
        auto d = delta(2); d["msg"]["delta_fp"] = q; bad.push_back(d);
    }
    auto missing = delta(2); missing["msg"].erase("market_ticker"); bad.push_back(missing);
    auto side = delta(2); side["msg"]["side"] = "bid"; bad.push_back(side);
    auto snap = snapshot(2); snap["msg"]["no_dollars_fp"] = json::array({json::array({"oops", "2.00"})}); bad.push_back(snap);
    snap = snapshot(2); snap["msg"]["yes_dollars_fp"].push_back({"0.5600", "2.00"}); bad.push_back(snap);
    bad.push_back("{not JSON"); bad.push_back(json::array());
    for (const auto& frame : bad) {
        SCOPED_TRACE(frame.dump());
        auto records = journal(json::array({snapshot(), frame, delta(3), snapshot(4), delta(5, "-10.25")}));
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        feed.apply(records[0]); EXPECT_NO_THROW(feed.apply(records[1]));
        EXPECT_FALSE(seen.invalid.empty()); EXPECT_FALSE(seen.book.valid);
        EXPECT_EQ(seen.snapshots, 1u); EXPECT_EQ(seen.updates, 0u);
        feed.apply(records[2]); EXPECT_EQ(seen.updates, 0u);
        feed.apply(records[3]); feed.apply(records[4]);
        EXPECT_EQ(seen.snapshots, 2u); EXPECT_EQ(seen.updates, 1u);
        EXPECT_TRUE(seen.book.bids.empty()); EXPECT_EQ(seen.ticks, 5u);
    }
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : journal(json::array({delta(1)}))) EXPECT_NO_THROW(feed.apply(r));
    EXPECT_TRUE(seen.invalid.empty()); EXPECT_EQ(seen.updates, 0u);
}
TEST(KalshiFeed, EveryYesAndComplementPriceHasIdenticalBits) {
    auto snap = snapshot(); snap["msg"].erase("yes_dollars_fp"); snap["msg"].erase("no_dollars_fp");
    const auto decimal = [](int p) { auto s = std::to_string(10000 + p); return std::to_string(p / 10000) + "." + s.substr(1); };
    for (int p = 0; p <= 10000; ++p) {
        snap["msg"]["yes_dollars_fp"].push_back({decimal(p), "1.01"});
        snap["msg"]["no_dollars_fp"].push_back({decimal(10000-p), "1.01"});
    }
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : journal(json::array({snap}))) feed.apply(r);
    ASSERT_TRUE(seen.invalid.empty()); ASSERT_EQ(seen.book.bids.size(), 10001u); ASSERT_EQ(seen.book.asks.size(), 10001u);
    auto ask = seen.book.asks.begin();
    for (const auto& [p, q] : seen.book.bids) {
        EXPECT_EQ(bits(p), bits(ask->first)); EXPECT_EQ(bits(q), bits(ask->second)); ++ask;
    }
}
TEST(KalshiFeed, StateRoundTripAtEveryCaptureBoundaryPreservesCallbacks) {
    for (const auto& product : {A, B}) {
        const auto records = journal(fixture("ws_capture_sample.json"), product);
        for (size_t cut = 1; cut < records.size(); ++cut) {
            SCOPED_TRACE(product + " cut " + std::to_string(cut));
            JournalFeed original(product), restored(product);
            Observer left, right; left.attach(original); right.attach(restored);
            for (size_t i = 0; i < cut; ++i) original.apply(records[i]);
            const auto state = original.exportState();
            restored.importState(json::from_cbor(json::to_cbor(state)));
            EXPECT_EQ(state.dump(), restored.exportState().dump()); EXPECT_TRUE(right.events.empty());
            left.events.clear();
            for (size_t i = cut; i < records.size(); ++i) {
                original.apply(records[i]); restored.apply(records[i]);
            }
            EXPECT_EQ(left.events, right.events); EXPECT_EQ(original.exportState(), restored.exportState());
        }
    }
}
TEST(KalshiFeed, TradeLossDoesNotInvalidateBooksAndSideFieldsAreChecked) {
    auto frames = fixture("ws_capture_sample.json");
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        if (it->at("type") == "trade") { frames.erase(it); break; }
    }
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : journal(frames)) feed.apply(r);
    EXPECT_TRUE(seen.invalid.empty()); EXPECT_TRUE(seen.book.valid);
    EXPECT_EQ(seen.trades.size(), 9u);
}
TEST(KalshiFeed, ClocksForeignMarketsAndEmptySides) {
    auto first = snapshot(); first["sending_ts_ms"] = Epoch - 20;
    auto second = delta(2); second["sending_ts_ms"] = Epoch - 10; second["msg"]["ts_ms"] = Epoch - 15;
    auto third = delta(3); third["msg"]["ts"] = "2026-10-24T16:40:39.021Z";
    auto empty = snapshot(5); empty["msg"].erase("yes_dollars_fp"); empty["msg"].erase("no_dollars_fp");
    auto foreign = delta(4, "-999.00", B); foreign["msg"]["price_dollars"] = "bad";
    const auto records = journal(json::array({first, second, third, foreign, empty, delta(6, "1.00")}));
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    feed.apply(records[0]); EXPECT_EQ(seen.book.envelopeMs, Epoch-20);
    feed.apply(records[1]); EXPECT_EQ(seen.book.envelopeMs, Epoch-15);
    feed.apply(records[2]); EXPECT_EQ(seen.book.envelopeMs, 1792860039021);
    feed.apply(records[3]); EXPECT_TRUE(seen.invalid.empty()); EXPECT_EQ(seen.updates, 2u);
    feed.apply(records[4]); EXPECT_TRUE(seen.book.bids.empty()); EXPECT_TRUE(seen.book.asks.empty());
    feed.apply(records[5]); EXPECT_EQ(seen.book.envelopeMs, Epoch+5); EXPECT_EQ(seen.book.bids.at(.56), 1);
}
TEST(KalshiFeed, LifecycleAndSubscriptionChangesRequireSnapshots) {
    auto records = journal(json::array({snapshot(), delta(2), snapshot(3), delta(4)}));
    for (const auto kind : {capture::Kind::TransportUp, capture::Kind::TransportDown,
                           capture::Kind::CaptureStopped, capture::Kind::BookInvalidated, capture::Kind::ResyncRequested}) {
        SCOPED_TRACE(int(kind));
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        feed.apply(records[0]);
        auto marker = records[1]; marker.record.kind = kind; marker.record.payload = "{}";
        feed.apply(marker); EXPECT_FALSE(seen.book.valid);
        feed.apply(records[1]); EXPECT_EQ(seen.updates, 0u);
        feed.apply(records[2]); feed.apply(records[3]); EXPECT_EQ(seen.updates, 1u);
        EXPECT_EQ(seen.ticks, 5u);
    }
    const json subscribed = {{"type", "subscribed"}, {"msg", {{"channel", "orderbook_delta"}, {"sid", 2}}}};
    auto changed = journal(json::array({snapshot(), subscribed, delta(1, "1.00", A, 2),
                                        snapshot(2, A, 2), delta(3, "1.00", A, 2), snapshot(99)}));
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : changed) feed.apply(r);
    EXPECT_EQ(seen.snapshots, 3u); EXPECT_EQ(seen.updates, 1u); EXPECT_EQ(seen.invalid.size(), 1u);
    EXPECT_EQ(seen.book.bids.at(.56), 10.25);
}
TEST(KalshiFeed, StateRejectsMalformedCandidatesAtomicallyAndRestoresInvalidState) {
    auto records = journal(json::array({snapshot(), delta(3), delta(4), snapshot(5), delta(6)}));
    JournalFeed feed(A); feed.apply(records[0]);
    const auto good = feed.exportState();
    for (int mode = 0; mode < 7; ++mode) {
        auto bad = good;
        if (mode == 0) bad["kalshi"]["yes"][0][0] = 10001;
        if (mode == 1) bad["kalshi"]["yes"][0][1] = -1;
        if (mode == 2) bad["kalshi"]["sequence"] = -1;
        if (mode == 3) bad["kalshi"]["sid"] = nullptr;
        if (mode == 4) bad["kalshi"]["valid"] = false;
        if (mode == 5) bad["kalshi"]["yes"].push_back(bad["kalshi"]["yes"][0]);
        if (mode == 6) bad["venue"] = "unsupported";
        EXPECT_THROW(feed.importState(bad), std::exception); EXPECT_EQ(feed.exportState(), good);
    }
    feed.apply(records[1]);
    JournalFeed restored(A); Observer left, right; left.attach(feed); right.attach(restored);
    restored.importState(feed.exportState()); EXPECT_TRUE(right.events.empty());
    for (size_t i = 2; i < records.size(); ++i) { feed.apply(records[i]); restored.apply(records[i]); }
    EXPECT_EQ(left.events, right.events); EXPECT_EQ(feed.exportState(), restored.exportState());
}
TEST(KalshiFeed, OverflowAndConsumerExceptionsAreNotSilentlyRepaired) {
    auto maximum = snapshot(); maximum["msg"]["yes_dollars_fp"] = json::array({json::array({"0.5600", "92233720368547758.07"})});
    auto records = journal(json::array({maximum, delta(2, "0.01")}));
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    feed.apply(records[0]); ASSERT_TRUE(seen.book.valid);
    JournalFeed restored(A); EXPECT_NO_THROW(restored.importState(feed.exportState()));
    feed.apply(records[1]); EXPECT_EQ(seen.invalid.size(), 1u); EXPECT_EQ(seen.updates, 0u);
    JournalFeed throwing(A);
    throwing.onSnapshot = [](auto, auto, auto) { throw std::runtime_error("consumer failed"); };
    EXPECT_THROW(throwing.apply(records[0]), std::logic_error);
}
TEST(KalshiFeed, SelectedSnapshotsReanchorSequenceAndRecoverRetiredSubscriptions) {
    for (const int seq : {1, 2, 5}) {
        SCOPED_TRACE(seq);
        auto records = journal(json::array({snapshot(), delta(2), snapshot(seq), delta(seq + 1)}));
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        for (const auto& r : records) feed.apply(r);
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_TRUE(seen.book.valid);
        EXPECT_EQ(seen.snapshots, 2u); EXPECT_EQ(seen.updates, 2u);
        EXPECT_EQ(seen.book.bids.at(.56), 11.5);
        EXPECT_EQ(feed.exportState()["kalshi"]["sequence"], seq + 1);
    }
    const json retired = {{"type", "unsubscribed"}, {"sid", 1}, {"seq", 2}};
    auto records = journal(json::array({snapshot(), retired, delta(3), snapshot(8), delta(9)}));
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    feed.apply(records[0]); feed.apply(records[1]); feed.apply(records[2]);
    EXPECT_FALSE(seen.book.valid); EXPECT_EQ(seen.invalid.size(), 1u); EXPECT_EQ(seen.updates, 0u);
    JournalFeed restored(A); Observer resumed; resumed.attach(restored); restored.importState(feed.exportState());
    seen.events.clear();
    for (size_t i = 3; i < records.size(); ++i) { feed.apply(records[i]); restored.apply(records[i]); }
    EXPECT_EQ(seen.events, resumed.events); EXPECT_EQ(feed.exportState(), restored.exportState());
    EXPECT_EQ(resumed.snapshots, 1u); EXPECT_EQ(resumed.updates, 1u); EXPECT_TRUE(resumed.book.valid);
}
TEST(KalshiFeed, TradeSideAlternativesAndMalformedFields) {
    auto source = fixture("ws_capture_sample.json");
    auto it = std::find_if(source.begin(), source.end(), [](const auto& f) { return f.at("type") == "trade"; });
    ASSERT_NE(it, source.end());
    for (const auto* field : {"taker_outcome_side", "taker_book_side"}) {
        auto trade = *it; trade["msg"].erase(field);
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        for (const auto& r : journal(json::array({snapshot(), trade, delta(2)}))) feed.apply(r);
        ASSERT_EQ(seen.trades.size(), 1u); EXPECT_EQ(seen.trades[0].side, AggressorSide::Sell);
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_EQ(seen.updates, 1u);
    }
    for (int mode = 0; mode < 10; ++mode) {
        SCOPED_TRACE(mode);
        auto trade = *it;
        if (mode == 0) trade["msg"]["taker_book_side"] = "bid";
        if (mode == 1) trade["msg"]["count_fp"] = "-1.00";
        if (mode == 2) trade["msg"]["yes_price_dollars"] = "0.93001";
        if (mode == 3) trade["msg"]["ts_ms"] = 9223372036854775807LL;
        if (mode == 4) trade["msg"]["trade_id"] = nullptr;
        if (mode == 5 || mode == 9) {
            trade["msg"].erase("taker_outcome_side"); trade["msg"].erase("taker_book_side");
            trade["msg"].erase("taker_side");
            if (mode == 9) trade["msg"]["taker_side"] = "bad";
        }
        if (mode == 6) trade["msg"]["ts_ms"] = "bad";
        if (mode == 7) trade["msg"]["count_fp"] = "1.001";
        if (mode == 8) trade.erase("msg");
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        for (const auto& r : journal(json::array({snapshot(), trade, delta(2)}))) EXPECT_NO_THROW(feed.apply(r));
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_TRUE(seen.trades.empty()); EXPECT_EQ(seen.updates, 1u);
        EXPECT_TRUE(seen.book.valid); EXPECT_EQ(seen.book.bids.at(.56), 11.5);
    }
}

TEST(KalshiFeed, WaitingForSnapshotIgnoresDeltasWithoutRepeatedInvalidation) {
    json frames = json::array({snapshot(), delta(2), delta(4)});
    for (int seq = 5; seq < 1005; ++seq) frames.push_back(delta(seq));
    frames.push_back(snapshot(2000)); frames.push_back(delta(2001));
    auto records = journal(frames);
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    feed.apply(records[0]); feed.apply(records[1]); EXPECT_EQ(seen.updates, 1u);
    seen.updates = 0;
    for (size_t i = 2; i < records.size() - 2; ++i) feed.apply(records[i]);
    EXPECT_EQ(seen.boundaries, 1u); EXPECT_EQ(seen.invalid.size(), 1u);
    EXPECT_EQ(seen.updates, 0u); EXPECT_FALSE(seen.book.valid);
    EXPECT_EQ(feed.exportState()["kalshi"]["sequence"], 1004);
    feed.apply(records[records.size()-2]); feed.apply(records.back());
    EXPECT_TRUE(seen.book.valid); EXPECT_EQ(seen.updates, 1u); EXPECT_EQ(seen.invalid.size(), 1u);

    // Before the first snapshot, only the sequence envelope is relevant.
    auto bad = delta(2); bad["msg"]["price_dollars"] = "bad"; bad["msg"]["ts_ms"] = "bad";
    JournalFeed empty(A); Observer waiting; waiting.attach(empty);
    for (const auto& r : journal(json::array({delta(1), bad, snapshot(3), delta(4)}))) empty.apply(r);
    EXPECT_EQ(waiting.boundaries, 1u); EXPECT_TRUE(waiting.invalid.empty()); EXPECT_EQ(waiting.updates, 1u);
}
TEST(KalshiFeed, UnannouncedSidSnapshotsAdoptAndDeltasInvalidateOnce) {
    for (bool snapshotFirst : {true, false}) {
        SCOPED_TRACE(snapshotFirst);
        auto records = journal(json::array({snapshot(), delta(2), snapshot(99, B, 77), delta(100, "1.00", B, 77),
            snapshotFirst ? snapshot(10, A, 2) : delta(10, "1.00", A, 2),
            delta(11, "1.00", A, 2), delta(12, "1.00", A, 2), snapshot(20, A, 2), delta(21, "1.00", A, 2)}));
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        for (size_t i = 0; i < 4; ++i) feed.apply(records[i]);
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_TRUE(seen.book.valid);
        EXPECT_EQ(feed.exportState()["kalshi"]["sequence"], 2);
        for (size_t i = 4; i < 7; ++i) feed.apply(records[i]);
        EXPECT_EQ(seen.invalid.size(), snapshotFirst ? 0u : 1u);
        EXPECT_EQ(seen.updates, snapshotFirst ? 3u : 1u); EXPECT_EQ(seen.book.valid, snapshotFirst);
        feed.apply(records[7]); feed.apply(records[8]);
        EXPECT_TRUE(seen.book.valid); EXPECT_EQ(seen.book.bids.at(.56), 11.25);
        EXPECT_EQ(feed.exportState()["kalshi"]["sid"], 2);
        EXPECT_EQ(feed.exportState()["kalshi"]["sequence"], 21);
    }
    // A foreign snapshot does not heal a gap in our subscription.
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : journal(json::array({snapshot(), snapshot(3, B), delta(4)}))) feed.apply(r);
    EXPECT_EQ(seen.invalid.size(), 1u); EXPECT_EQ(seen.snapshots, 1u); EXPECT_EQ(seen.updates, 0u);
}
TEST(KalshiFeed, LegacyTradeSideIsUsedOnlyWithoutModernFields) {
    const auto frames = fixture("ws_capture_sample.json");
    const auto source = *std::find_if(frames.begin(), frames.end(), [](const auto& f) { return f.at("type") == "trade"; });
    for (const auto* side : {"yes", "no"}) {
        auto trade = source; trade["msg"].erase("taker_outcome_side"); trade["msg"].erase("taker_book_side");
        trade["msg"]["taker_side"] = side;
        JournalFeed feed(A); Observer seen; seen.attach(feed);
        for (const auto& r : journal(json::array({snapshot(), trade}))) feed.apply(r);
        ASSERT_EQ(seen.trades.size(), 1u);
        EXPECT_EQ(seen.trades[0].side, std::string_view(side) == "yes" ? AggressorSide::Buy : AggressorSide::Sell);
        EXPECT_TRUE(seen.invalid.empty()); EXPECT_TRUE(seen.book.valid);
    }
    auto modern = source; modern["msg"]["taker_side"] = "yes"; // Modern fields say no/ask.
    JournalFeed feed(A); Observer seen; seen.attach(feed);
    for (const auto& r : journal(json::array({modern}))) feed.apply(r);
    ASSERT_EQ(seen.trades.size(), 1u); EXPECT_EQ(seen.trades[0].side, AggressorSide::Sell);
}
TEST(KalshiFeed, ReaderPreservesOpaqueMetadataAndExposesTypedHeaderIdentity) {
    for (const bool kalshi : {false, true}) {
        auto header = metadata(A);
        if (!kalshi) {
            header.erase("venue"); header.erase("instrument");
            header["product_metadata"]["quote_increment"] = "0.01";
            header["product_metadata"]["base_increment"] = "0.01";
        }
        header["product_metadata"]["_journal_venue"] = "opaque, not routing";
        header["product_metadata"]["extra"] = {{"nested", json::array({1, "two"})}};
        const auto records = journal(json::array({snapshot(), delta(2)}), A, header);
        for (const auto& r : records) {
            EXPECT_EQ(r.venue, kalshi ? Venue::Kalshi : Venue::Coinbase);
            EXPECT_EQ(r.metadata, header["product_metadata"]);
            EXPECT_EQ(r.instrument, header.value("instrument", json()));
        }
    }
    // Existing fanout aggregate initializers retain their original field order/defaults.
    JournalRecord synthetic{{capture::Kind::Frame, {Epoch * 1'000'000, 0}, 1, "{}"}, {A, "run", 0, 0}, {}, false, 1};
    EXPECT_EQ(synthetic.venue, Venue::Coinbase); EXPECT_TRUE(synthetic.instrument.is_null());
}
TEST(KalshiFeed, VenueChangesInvalidateIncludingAfterStateImport) {
    auto kalshi = journal(json::array({snapshot(), delta(2)}));
    const auto coinFrame = [&](const char* type, int seq) {
        return json{{"channel", "l2_data"}, {"sequence_num", seq}, {"events", json::array({
            {{"type", type}, {"product_id", A}, {"updates", json::array({
                {{"side", "bid"}, {"price_level", "100"}, {"new_quantity", "2"}}})}}})}};
    };
    const json coinHeader = {{"product_metadata", {{"product_id", A}, {"quote_increment", "0.01"}, {"base_increment", "0.01"}}}};
    auto coin = journal(json::array({coinFrame("snapshot", 1), coinFrame("update", 2)}), A, coinHeader);
    // Isolate venue identity from journal run/connection changes.
    for (auto* records : {&coin, &kalshi}) for (auto& r : *records) { r.pos.run = "same-run"; r.gapBefore = false; }
    for (const bool fromKalshi : {false, true}) for (const bool imported : {false, true}) {
        SCOPED_TRACE(std::to_string(fromKalshi) + "/" + std::to_string(imported));
        auto& before = fromKalshi ? kalshi : coin; auto& after = fromKalshi ? coin : kalshi;
        JournalFeed original(A), restored(A); Observer seen;
        original.apply(before[0]);
        if (imported) restored.importState(original.exportState());
        auto& feed = imported ? restored : original; seen.attach(feed);
        feed.apply(after[1]); // An update from the other venue cannot reuse the old snapshot.
        ASSERT_EQ(seen.invalid.size(), 1u); EXPECT_EQ(seen.invalid[0], "journal venue changed");
        EXPECT_EQ(seen.boundaries, 0u); EXPECT_EQ(seen.updates, 0u); EXPECT_FALSE(seen.book.valid);
        auto snap = after[0], next = after[1];
        if (fromKalshi) {
            snap.record.payload = coinFrame("snapshot", 3).dump(); next.record.payload = coinFrame("update", 4).dump();
        }
        feed.apply(snap); feed.apply(next);
        EXPECT_TRUE(seen.book.valid); EXPECT_EQ(seen.updates, 1u);
        EXPECT_EQ(seen.book.bids.size(), 1u);
        EXPECT_TRUE(seen.book.bids.contains(fromKalshi ? 100 : .56));
        EXPECT_EQ(feed.exportState()["version"], fromKalshi ? 1 : 2);
    }
}
