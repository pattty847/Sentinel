// Offline measurement of one flow history page, as the server builds it
// (FlowStore::window reads and decodes whole FLD1 day files, then
// SentinelStreamServer composes each coarse bucket and encodes FLB1 frames).
// Read-only: it opens FLD1 files and RAWL2 journals for reading and writes
// nothing. Output is key=value lines.
//
//   flow_history_bench <fld1-root> <symbol> <end: ms | YYYY-MM-DDTHH:MM:SSZ>
//                      [--journal <rawl2-root> --day YYYY-MM-DD] [--passes N]
//
// Page mode: for 1m, 1h and 1d, the 512-bar page whose exclusive end is the end
// time floored to the timeframe, clamped to the first FLD1 day on disk. Every
// day file the page touches is read whole, as the server does, and every
// requested bucket gets the server's range/bar JSON and FHE1 envelope work,
// missing days included. FlowStore itself is not used: without a build pass its
// sealed watermark is zero and window() returns no bars. measured_subset_ms is
// therefore a named subset; the program prints which server stages it leaves out.
// Pass 1 may hit a cold OS cache; later passes are warm.
// Journal mode (--journal, --day): one UTC day of RAWL2 replayed through
// JournalFeed (every frame, FM-278) for its trades, against reading and decoding
// that day's FLD1 file.
//
//   flow_history_bench --lateness <rawl2-root> <symbol> <YYYY-MM-DD>...
//
// Lateness mode (docs/FLOW_CHUNKS.md section 7): every trade whose exchange
// time is in the day, deduplicated and ordered by JournalFeed as FlowStore does,
// with the receive time FlowStore uses (the RAWL2 record's systemNs). One hour of
// lead-in, and reading until the running maximum receive time is an hour past
// the day. Prints receive - exchange and receive - end of the trade's 1m bucket
// as histograms and quantiles, what a 15 s and a 60 s seal window exclude under
// the running-maximum rule, receive-time regressions, damage and completeness.
//
//   flow_history_bench --columnar <fld1-root> <symbol> <YYYY-MM-DD>...
//
// Columnar mode: re-lays FLD1 days in the section 4 payload and the section 5
// day file of docs/FLOW_CHUNKS.md (flow_chunk_proto.hpp) and prints sizes raw
// and with zstd against FLB1, read cost, 1h and assembled 16-day 1d chunks, row
// density, int16 offset headroom and big-trade floor counts over every FLD1 day.
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include "flow_chunk_proto.hpp"
#include "roller/JournalFeed.hpp"
#include "heatmap/ChunkCodec.hpp"
#include <zstd.h>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sentinel;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
constexpr int64_t PageBars = 512;
// The receive-time tail FlowStore replays past a day's end (FlowStore.cpp TradeTail).
constexpr int64_t TradeTail = Minute;
// Footer and CVD read four integers per bar (buyVol, sellVol, delta,
// baseVolume) plus the bucket start.
constexpr size_t SummaryBytesPerBar = 5 * sizeof(int64_t);

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
int64_t parseDay(const std::string& text) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d", &y, &m, &d) != 3) throw std::invalid_argument("invalid date " + text);
    const auto day = std::chrono::year{y} / std::chrono::month{unsigned(m)} / std::chrono::day{unsigned(d)};
    if (!day.ok()) throw std::invalid_argument("invalid date " + text);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days{day}.time_since_epoch()).count();
}
int64_t parseTime(const std::string& text) {
    if (text.find('-') == std::string::npos) return std::stoll(text);
    int hh = 0, mm = 0, ss = 0;
    const auto t = text.find('T');
    if (t != std::string::npos && std::sscanf(text.c_str() + t + 1, "%d:%d:%d", &hh, &mm, &ss) < 2)
        throw std::invalid_argument("invalid time " + text);
    return parseDay(text.substr(0, t)) + ((hh * 60 + mm) * 60 + ss) * 1000LL;
}
std::string dayName(int64_t dayMs) {
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{dayMs}})};
    char out[16];
    std::snprintf(out, sizeof out, "%04d-%02u-%02u", int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()));
    return out;
}
fs::path dayPath(const fs::path& root, const std::string& symbol, int64_t dayMs) {
    return root / symbol / "60000" / (dayName(dayMs) + ".flb");
}
std::optional<int64_t> firstDay(const fs::path& root, const std::string& symbol) {
    std::optional<int64_t> first;
    for (const auto& entry : fs::directory_iterator(root / symbol / "60000")) {
        if (entry.path().extension() != ".flb") continue;
        try {
            const auto day = parseDay(entry.path().stem().string());
            if (!first || day < *first) first = day;
        } catch (const std::exception&) {}
    }
    return first;
}
std::vector<uint8_t> readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    in.seekg(0, std::ios::end);
    std::vector<uint8_t> bytes(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!in) throw std::runtime_error("short read " + path.string());
    return bytes;
}

struct PageStats {
    int64_t fromMs = 0, toMs = 0;
    int days = 0, missingDays = 0, buckets = 0, firstTouchDays = 0;
    size_t bytesRead = 0, barsIn = 0, barsOut = 0, rowsOut = 0, encodedBytes = 0, envelopeBytes = 0;
    size_t summaryBytes = 0;
    double readMs = 0, decodeMs = 0, validateMs = 0, composeMs = 0, encodeMs = 0, jsonMs = 0, envelopeMs = 0;
    double summaryMs = 0, indexLoadMs = 0;
    double subsetMs() const { return readMs + decodeMs + validateMs + composeMs + encodeMs + jsonMs + envelopeMs; }
};
struct Summary { int64_t buy = 0, sell = 0, delta = 0, base = 0; };
using Json = nlohmann::json;
struct SliceRange { int64_t fromMs, toMs; const char* status; };

// FlowStore::loadIndex on the first touch of a day in a process (cached after):
// read the .state sidecar, SHA-256 over state and day bytes, a validated decode.
void indexLoad(PageStats& s, const fs::path& path, const std::vector<uint8_t>& bytes) {
    const auto start = Clock::now();
    const auto state = readAll(path.string() + ".state");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(state.data()), qsizetype(state.size())));
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())));
    (void)hash.result();
    (void)flow::decodeFlowDay(bytes);
    s.indexLoadMs += msSince(start);
}

// A copy of SentinelStreamServer flow_history::rangeStatus.
std::string rangeStatus(const Json& ranges) {
    bool building = false, partial = false, unrecorded = false, complete = false;
    for (const auto& range : ranges) {
        const auto status = range.at("status").get<std::string>();
        building |= status == "building";
        partial |= status == "partial";
        unrecorded |= status == "unrecorded";
        complete |= status == "complete";
    }
    return building ? "building" : partial || (unrecorded && complete) ? "partial" :
        unrecorded ? "unrecorded" : "complete";
}

// One UTC day slice [from, to) of the page, modelled on SentinelStreamServer
// flow_history::buildPage: one hourly store range per hour (complete for a day
// file on disk, unrecorded for a missing day), then per requested bucket its
// range status, range and bar metadata, and composition of the bucket's bars.
// indexed: days whose index this run already loaded. The server caches
// indexFor(symbol, day) for the life of the process, so only a first touch pays.
void pageDay(PageStats& s, Json& header, std::vector<flow::FlowBar>& out, const fs::path& path,
             int64_t from, int64_t to, int64_t tf, std::set<fs::path>& indexed) {
    std::map<int64_t, std::vector<std::pair<flow::FlowBar, uint32_t>>> buckets;
    const bool present = fs::exists(path);
    if (present) {
        ++s.days;
        auto start = Clock::now();
        const auto bytes = readAll(path);
        s.readMs += msSince(start);
        s.bytesRead += bytes.size();
        start = Clock::now();
        auto day = flow::decodeFlowDay(bytes, flow::DayCheck::BarsOnly);
        s.decodeMs += msSince(start);
        start = Clock::now();
        if (flow::encodeFlowDay(day) != bytes) throw std::runtime_error("day re-encode differs " + path.string());
        s.validateMs += msSince(start);
        if (indexed.insert(path).second) {
            indexLoad(s, path, bytes);
            ++s.firstTouchDays;
        }
        for (size_t i = 0; i < day.bars.size(); ++i) {
            auto& bar = day.bars[i];
            if (bar.startMs >= from && bar.startMs < to)
                buckets[bar.startMs / tf * tf].emplace_back(std::move(bar), day.revisions[i]);
        }
        for (const auto& [_, parts] : buckets) s.barsIn += parts.size();
        // Footer-only pass: kept out of the subset total.
        start = Clock::now();
        std::map<int64_t, Summary> summaries;
        for (const auto& [bucket, parts] : buckets) {
            auto& sum = summaries[bucket];
            for (const auto& [bar, _] : parts) {
                sum.buy += bar.buyVol; sum.sell += bar.sellVol; sum.delta += bar.delta; sum.base += bar.baseVolume;
            }
        }
        s.summaryMs += msSince(start);
        s.summaryBytes += summaries.size() * SummaryBytesPerBar;
    } else {
        ++s.missingDays;
    }
    std::vector<SliceRange> ranges;
    for (int64_t hour = from / Hour * Hour; hour < to; hour += Hour)
        ranges.push_back({std::max(hour, from), std::min(hour + Hour, to), present ? "complete" : "unrecorded"});
    double composeMs = 0;
    const auto start = Clock::now();
    for (int64_t bucket = from / tf * tf; bucket < to; bucket += tf) {
        ++s.buckets;
        const int64_t end = bucket + tf;
        int64_t covered = bucket;
        bool building = false, unrecorded = false, recorded = false;
        for (const auto& r : ranges) {
            if (r.toMs <= bucket || r.fromMs >= end) continue;
            if (r.fromMs > covered) building = true;
            covered = std::max(covered, r.toMs);
            unrecorded |= std::string_view(r.status) == "unrecorded";
            recorded |= std::string_view(r.status) != "unrecorded";
        }
        building |= covered < end;
        const bool partial = unrecorded && recorded;
        const std::string status = building ? "building" : partial ? "partial" : unrecorded ? "unrecorded" : "complete";
        header["ranges"].push_back({{"from_ms", bucket}, {"to_ms", end}, {"status", status},
                                    {"price_scale", 100}, {"row_tick_units", 100}});
        auto it = buckets.find(bucket);
        if (it == buckets.end()) continue;
        auto& entries = it->second;
        const auto composeStart = Clock::now();
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first.startMs < b.first.startMs; });
        std::vector<flow::FlowBar> parts;
        uint32_t revision = 0;
        for (auto& [bar, rev] : entries) { parts.push_back(std::move(bar)); revision = std::max(revision, rev); }
        out.push_back(tf == parts.front().timeframeMs ? std::move(parts.front()) : flow::composeFlowBars(parts, tf));
        composeMs += msSince(composeStart);
        header["bars"].push_back({{"bucket_start_ms", bucket}, {"revision", revision},
                                  {"complete", !building && !partial}, {"source", "store"}});
    }
    s.composeMs += composeMs;
    s.jsonMs += msSince(start) - composeMs;
}

PageStats page(const fs::path& root, const std::string& symbol, int64_t endMs, int64_t tf, int64_t firstDayMs,
               std::set<fs::path>& indexed) {
    PageStats s;
    s.toMs = endMs / tf * tf;
    s.fromMs = std::max(s.toMs - PageBars * tf, firstDayMs / tf * tf);
    Json header = {{"type", "flow_history_chunk"}, {"schema_version", 1}, {"req", 1}, {"symbol", symbol},
                   {"timeframe_ms", tf}, {"oldest_ms", s.fromMs}, {"price_scale", 0}, {"row_tick_units", 0},
                   {"sealed_through_ms", s.toMs}, {"ranges", Json::array()}, {"bars", Json::array()}};
    std::vector<flow::FlowBar> composed;
    // The server bounds source memory to one UTC day per buildPage call.
    for (int64_t day = s.fromMs / Day * Day; day < s.toMs; day += Day)
        pageDay(s, header, composed, dayPath(root, symbol, day), std::max(s.fromMs, day), std::min(s.toMs, day + Day), tf,
                indexed);
    auto start = Clock::now();
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(composed.size());
    for (const auto& bar : composed) {
        frames.push_back(flow::encodeFlowBar(bar));
        s.encodedBytes += 4 + frames.back().size(); // u32 length prefix per frame
        s.rowsOut += bar.rows.size();
    }
    s.encodeMs = msSince(start);
    s.barsOut = composed.size();
    // FHE1: magic, u32 JSON length, JSON header, length-prefixed FLB1 frames.
    start = Clock::now();
    header["status"] = rangeStatus(header["ranges"]);
    std::string body;
    for (const auto& bytes : frames) {
        for (unsigned i = 0; i < 4; ++i) body.push_back(char(uint32_t(bytes.size()) >> (8 * i)));
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    const auto json = header.dump();
    std::string envelope = "FHE1";
    for (unsigned i = 0; i < 4; ++i) envelope.push_back(char(uint32_t(json.size()) >> (8 * i)));
    envelope += json + body;
    s.envelopeMs = msSince(start);
    s.envelopeBytes = envelope.size();
    return s;
}

void printPage(const std::string& symbol, int64_t tf, int pass, const PageStats& s) {
    std::cout << "mode=page symbol=" << symbol << " tf_ms=" << tf << " pass=" << pass
              << " from_ms=" << s.fromMs << " to_ms=" << s.toMs << " buckets=" << s.buckets
              << " day_files=" << s.days << " missing_days=" << s.missingDays
              << " bytes_read=" << s.bytesRead << " read_ms=" << s.readMs
              << " decode_ms=" << s.decodeMs << " validate_reencode_ms=" << s.validateMs
              << " compose_ms=" << s.composeMs << " encode_ms=" << s.encodeMs << " json_ms=" << s.jsonMs
              << " envelope_ms=" << s.envelopeMs << " measured_subset_ms=" << s.subsetMs()
              << " bars_in=" << s.barsIn << " bars_out=" << s.barsOut << " rows_out=" << s.rowsOut
              << " encoded_bytes=" << s.encodedBytes << " envelope_bytes=" << s.envelopeBytes
              << " first_touch_days=" << s.firstTouchDays << " first_touch_index_ms=" << s.indexLoadMs
              << " summary_bytes=" << s.summaryBytes << " summary_ms=" << s.summaryMs
              << " summary_share=" << (s.envelopeBytes ? double(s.summaryBytes) / double(s.envelopeBytes) : 0.0) << '\n';
}

void journalDay(const fs::path& journalRoot, const fs::path& flowRoot, const std::string& symbol, int64_t dayMs) {
    const int64_t end = dayMs + Day;
    auto start = Clock::now();
    roller::JournalReader catalog(journalRoot, symbol);
    const double catalogMs = msSince(start);
    if (catalog.files().empty()) throw std::runtime_error("no journal files for " + symbol);
    size_t chosen = 0;
    for (size_t i = 0; i < catalog.files().size(); ++i) {
        if (catalog.files()[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= dayMs) chosen = i;
        else break;
    }
    size_t journalBytes = 0;
    int journalFiles = 0;
    for (size_t i = chosen; i < catalog.files().size(); ++i) {
        if (catalog.files()[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 > end + TradeTail) break;
        journalBytes += fs::file_size(catalog.files()[i].path);
        ++journalFiles;
    }
    const auto& h = catalog.files()[chosen].header;
    const roller::JournalPos pos{symbol, h.at("run_id").get<std::string>(), h.at("first_block_ordinal").get<uint64_t>(), 0};

    // Trades: every frame goes through JournalFeed (dedupe and order need the l2 frames too).
    start = Clock::now();
    roller::JournalReader reader(catalog, pos);
    roller::JournalFeed feed(symbol);
    uint64_t trades = 0, records = 0;
    feed.onTrade = [&](const ::Trade& t) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count();
        trades += ms >= dayMs && ms < end;
    };
    roller::JournalRecord record;
    while (reader.next(record)) {
        if (record.record.time.systemNs / 1'000'000 >= end + TradeTail) break;
        ++records;
        feed.apply(record);
    }
    const double tradeScanMs = msSince(start);

    // Block decode alone, without the JSON parse in JournalFeed (warm cache now).
    start = Clock::now();
    roller::JournalReader rawReader(catalog, pos);
    uint64_t rawRecords = 0;
    while (rawReader.next(record)) {
        if (record.record.time.systemNs / 1'000'000 >= end + TradeTail) break;
        ++rawRecords;
    }
    const double decodeOnlyMs = msSince(start);

    const auto path = dayPath(flowRoot, symbol, dayMs);
    start = Clock::now();
    const auto bytes = readAll(path);
    const double fldReadMs = msSince(start);
    start = Clock::now();
    const auto day = flow::decodeFlowDay(bytes);
    const double fldDecodeMs = msSince(start);
    start = Clock::now();
    const auto barsOnly = flow::decodeFlowDay(bytes, flow::DayCheck::BarsOnly);
    const double fldBarsOnlyMs = msSince(start);
    uint64_t fldTrades = 0;
    for (const auto& bar : barsOnly.bars) fldTrades += bar.count;

    std::cout << "mode=journal symbol=" << symbol << " day=" << dayName(dayMs)
              << " catalog_ms=" << catalogMs << " journal_files=" << journalFiles
              << " journal_bytes=" << journalBytes << " records=" << records
              << " trades=" << trades << " trade_scan_ms=" << tradeScanMs
              << " decode_only_records=" << rawRecords << " decode_only_ms=" << decodeOnlyMs
              << " fld_bytes=" << bytes.size() << " fld_read_ms=" << fldReadMs
              << " fld_decode_validated_ms=" << fldDecodeMs << " fld_decode_bars_only_ms=" << fldBarsOnlyMs
              << " fld_bars=" << day.bars.size() << " fld_trades=" << fldTrades << '\n';
}
// Quantile of a sorted vector (nearest rank).
int64_t quantile(const std::vector<int64_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    const auto rank = size_t(std::ceil(q * double(sorted.size())));
    return sorted[std::min(sorted.size() - 1, rank ? rank - 1 : 0)];
}

struct LateTrade { int64_t afterEndMs, exchangeMs, receiveMs; bool snapshotFrame; std::string id; };

// The seal rule of docs/FLOW_CHUNKS.md section 7: a trade misses its bar when the
// running maximum receive time over all records, advanced by its own record
// before the record's trades apply, is at or past bar end + W.
void latenessDay(const fs::path& journalRoot, const std::string& symbol, int64_t dayMs) {
    // An hour of lead-in warms JournalFeed's trade-id window and the running
    // maximum; reading stops once the running maximum is an hour past the day.
    constexpr int64_t LeadIn = Hour, ReadTail = Hour;
    const int64_t end = dayMs + Day;
    roller::JournalReader catalog(journalRoot, symbol);
    if (catalog.files().empty()) throw std::runtime_error("no journal files for " + symbol);
    // Reader order is header order; count files whose open time goes backwards in it.
    size_t chosen = 0;
    uint64_t fileOrderRegressions = 0;
    int64_t previousOpen = INT64_MIN;
    for (size_t i = 0; i < catalog.files().size(); ++i) {
        const auto opened = catalog.files()[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000;
        fileOrderRegressions += opened < previousOpen;
        previousOpen = std::max(previousOpen, opened);
        if (opened <= dayMs - LeadIn) chosen = i;
    }
    const auto& h = catalog.files()[chosen].header;
    const roller::JournalPos pos{symbol, h.at("run_id").get<std::string>(), h.at("first_block_ordinal").get<uint64_t>(), 0};
    const auto start = Clock::now();
    roller::JournalReader reader(catalog, pos);
    roller::JournalFeed feed(symbol);
    int64_t receiveMs = 0, seenMs = INT64_MIN; // this record's receive time; running maximum
    bool snapshotFrame = false;
    uint64_t records = 0, transportUps = 0, regressions = 0, runRegressions = 0;
    int64_t maxRegressionMs = 0, firstReceive = INT64_MAX;
    std::string run;
    std::vector<int64_t> transit, afterEnd; // receive - exchange; receive - bucket end (own record time)
    struct Late { uint64_t own15 = 0, own60 = 0, max15 = 0, max60 = 0; };
    std::map<int64_t, Late> barLate;
    std::vector<LateTrade> worst;
    uint64_t lateSnapshot = 0, lateUpdate = 0; // trades more than 1 s after their bar ended
    feed.onTrade = [&](const ::Trade& t) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count();
        if (ms < dayMs || ms >= end) return;
        const int64_t bucketEnd = ms / Minute * Minute + Minute;
        const int64_t after = receiveMs - bucketEnd, afterMax = seenMs - bucketEnd;
        transit.push_back(receiveMs - ms);
        afterEnd.push_back(after);
        if (after > 1000) (snapshotFrame ? lateSnapshot : lateUpdate) += 1;
        auto& late = barLate[bucketEnd - Minute];
        late.own15 += after >= 15'000; late.own60 += after >= 60'000;
        late.max15 += afterMax >= 15'000; late.max60 += afterMax >= 60'000;
        if (worst.size() < 8 || afterMax > worst.back().afterEndMs) {
            worst.push_back({afterMax, ms, receiveMs, snapshotFrame, t.trade_id});
            std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.afterEndMs > b.afterEndMs; });
            if (worst.size() > 8) worst.pop_back();
        }
    };
    roller::JournalRecord record;
    while (reader.next(record)) {
        receiveMs = record.record.time.systemNs / 1'000'000;
        if (seenMs != INT64_MIN && receiveMs < seenMs) {
            ++regressions;
            maxRegressionMs = std::max(maxRegressionMs, seenMs - receiveMs);
            runRegressions += record.pos.run != run;
        }
        run = record.pos.run;
        seenMs = std::max(seenMs, receiveMs); // advanced before this record's trades
        if (seenMs >= end + ReadTail) break;
        ++records;
        firstReceive = std::min(firstReceive, receiveMs);
        transportUps += record.record.kind == capture::Kind::TransportUp;
        const auto& payload = record.record.payload;
        snapshotFrame = record.record.kind == capture::Kind::Frame &&
                        payload.find("market_trades") != std::string::npos &&
                        payload.find("\"type\":\"snapshot\"") != std::string::npos;
        feed.apply(record);
    }
    const double scanMs = msSince(start);
    // Complete: no damage, the lead-in reached, and the running maximum reached the read tail.
    const bool complete = !reader.damaged() && firstReceive <= dayMs - Minute && seenMs >= end + ReadTail;
    std::sort(transit.begin(), transit.end());
    std::sort(afterEnd.begin(), afterEnd.end());
    const auto bins = [](const std::vector<int64_t>& v) {
        // Bins: <0, [0,100 ms), [100 ms,1 s), [1,5 s), [5,15 s), [15,60 s), >=60 s.
        const std::array<int64_t, 6> edges{0, 100, 1000, 5000, 15'000, 60'000};
        std::array<uint64_t, 7> n{};
        for (auto x : v) ++n[size_t(std::upper_bound(edges.begin(), edges.end(), x) - edges.begin())];
        return n;
    };
    const auto print = [&](const char* name, const std::vector<int64_t>& v) {
        const auto n = bins(v);
        std::cout << "mode=lateness symbol=" << symbol << " day=" << dayName(dayMs) << " measure=" << name
                  << " trades=" << v.size() << " lt0=" << n[0] << " 0_100ms=" << n[1] << " 100ms_1s=" << n[2]
                  << " 1_5s=" << n[3] << " 5_15s=" << n[4] << " 15_60s=" << n[5] << " ge60s=" << n[6]
                  << " min_ms=" << (v.empty() ? 0 : v.front()) << " p50_ms=" << quantile(v, 0.5)
                  << " p99_ms=" << quantile(v, 0.99) << " p999_ms=" << quantile(v, 0.999)
                  << " max_ms=" << (v.empty() ? 0 : v.back()) << '\n';
    };
    print("receive_minus_exchange", transit);
    print("receive_minus_1m_bar_end", afterEnd);
    Late total;
    uint64_t bars15 = 0, bars60 = 0;
    for (const auto& [_, n] : barLate) {
        total.own15 += n.own15; total.own60 += n.own60; total.max15 += n.max15; total.max60 += n.max60;
        bars15 += n.max15 > 0; bars60 += n.max60 > 0;
    }
    std::cout << "mode=lateness symbol=" << symbol << " day=" << dayName(dayMs) << " scan_complete=" << complete
              << " damaged=" << reader.damaged() << " lead_in_ms=" << (dayMs - firstReceive)
              << " file_order_regressions=" << fileOrderRegressions << " receive_regressions=" << regressions
              << " cross_run_regressions=" << runRegressions << " max_regression_ms=" << maxRegressionMs
              << " excluded_15s_trades=" << total.max15 << " excluded_15s_bars=" << bars15
              << " excluded_60s_trades=" << total.max60 << " excluded_60s_bars=" << bars60
              << " own_time_excluded_15s=" << total.own15 << " own_time_excluded_60s=" << total.own60
              << " late_gt1s_in_snapshot_frames=" << lateSnapshot << " late_gt1s_in_update_frames=" << lateUpdate
              << " records=" << records << " transport_ups=" << transportUps
              << " first_receive_ms=" << firstReceive << " last_seen_ms=" << seenMs << " scan_ms=" << scanMs << '\n';
    for (const auto& w : worst)
        std::cout << "mode=lateness symbol=" << symbol << " day=" << dayName(dayMs)
                  << " worst_running_max_after_end_ms=" << w.afterEndMs << " exchange_ms=" << w.exchangeMs
                  << " receive_ms=" << w.receiveMs << " snapshot_frame=" << w.snapshotFrame << " trade_id=" << w.id << '\n';
}

// ---- Columnar mode -------------------------------------------------------
namespace proto = flow::proto;

size_t zstdSize(const std::vector<uint8_t>& raw, int level) {
    if (raw.empty()) return 0;
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    const auto n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(n)) throw std::runtime_error("zstd failed");
    return n;
}
// Raw, zstd 1, zstd 3 sums over independently compressed pieces, and how many
// pieces zstd 1 shrinks by at least 10% (the section 3 codec rule).
struct Size3 {
    size_t raw = 0, z1 = 0, z3 = 0, pieces = 0, pays = 0;
    void add(const std::vector<uint8_t>& bytes) {
        const auto a = zstdSize(bytes, 1);
        raw += bytes.size(); z1 += a; z3 += zstdSize(bytes, 3);
        ++pieces; pays += !bytes.empty() && double(a) <= 0.9 * double(bytes.size());
    }
};
std::string fmt(const char* name, const Size3& s, size_t bars) {
    std::ostringstream o;
    o << ' ' << name << "_raw=" << s.raw << ' ' << name << "_z1=" << s.z1 << ' ' << name << "_z3=" << s.z3
      << ' ' << name << "_z1_pays=" << s.pays << '/' << s.pieces;
    if (bars) o << ' ' << name << "_raw_per_bar=" << double(s.raw) / double(bars);
    return o.str();
}
flow::FlowBar stripNotables(flow::FlowBar b) { b.notable.clear(); return b; }

// Today's form for a set of bars: FLB1 frames plus the 24-byte FLD1 index entry each.
std::vector<uint8_t> flb1Form(std::span<const flow::FlowBar> bars, bool notables) {
    std::vector<uint8_t> out;
    for (const auto& b : bars) {
        const auto frame = flow::encodeFlowBar(notables ? b : stripNotables(b));
        out.insert(out.end(), 24, 0);
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}
struct LeanSizes { Size3 flb1, flb1NoNotables, fixed, varint, spec, summary; size_t bars = 0; };

struct Checks { proto::Stats stats; uint64_t roundTrip = 0, assembly = 0, dayFile = 0; };

// One chunk at one level: the spec payload (flow.rows), its summary prefix
// (flow.summary) and the two measurement variants; checks the spec round trip.
std::vector<uint8_t> chunkSizes(LeanSizes& s, std::span<const flow::FlowBar> bars, int64_t level, int64_t start,
                                uint32_t slotCount, Checks& checks) {
    s.bars += bars.size();
    s.flb1.add(flb1Form(bars, true));
    s.flb1NoNotables.add(flb1Form(bars, false));
    const auto chunk = proto::fromBars(bars, level, start, slotCount, &checks.stats);
    s.fixed.add(proto::encode(chunk, proto::Encoding::Fixed));
    s.varint.add(proto::encode(chunk, proto::Encoding::Varint));
    auto spec = proto::encode(chunk, proto::Encoding::Spec);
    s.spec.add(spec);
    s.summary.add(proto::encode(chunk, proto::Encoding::Spec, true));
    const auto back = proto::toBars(proto::decode(spec, proto::Encoding::Spec, level, start, slotCount));
    if (back.size() != bars.size()) checks.roundTrip += bars.size();
    else for (size_t i = 0; i < bars.size(); ++i) checks.roundTrip += !(back[i] == stripNotables(bars[i]));
    return spec;
}
void printLean(const std::string& symbol, const std::string& label, const LeanSizes& s) {
    std::cout << "mode=columnar symbol=" << symbol << ' ' << label << " bars=" << s.bars
              << fmt("flb1", s.flb1, s.bars) << fmt("flb1_no_notables", s.flb1NoNotables, s.bars)
              << fmt("variant_fixed", s.fixed, s.bars) << fmt("variant_varint", s.varint, s.bars)
              << fmt("spec_rows", s.spec, s.bars) << fmt("spec_summary", s.summary, s.bars) << '\n';
}

struct Density {
    std::vector<double> ratio;
    uint64_t rows = 0, span = 0;
    int64_t widest = 0;
    std::string widestDay;
    void add(const flow::FlowBar& b, const std::string& day) {
        if (b.rows.empty()) return;
        const int64_t width = int64_t(b.rows.back().offset) + 1;
        rows += b.rows.size(); span += uint64_t(width);
        ratio.push_back(double(b.rows.size()) / double(width));
        if (width > widest) { widest = width; widestDay = day; }
    }
};
void printDensity(const std::string& symbol, const char* level, Density& d) {
    std::sort(d.ratio.begin(), d.ratio.end());
    const auto q = [&](double p) { return d.ratio.empty() ? 0.0 : d.ratio[std::min(d.ratio.size() - 1, size_t(p * double(d.ratio.size())))]; };
    // Sparse = int16 offset + 24-byte record per present row; dense = 24 bytes per row slot of the range.
    std::cout << "mode=density symbol=" << symbol << " level=" << level << " bars=" << d.ratio.size()
              << " rows=" << d.rows << " row_slots=" << d.span
              << " density_total=" << (d.span ? double(d.rows) / double(d.span) : 0.0)
              << " density_p10=" << q(0.1) << " density_p50=" << q(0.5) << " density_p90=" << q(0.9)
              << " sparse_fixed_bytes=" << d.rows * 26 << " dense_fixed_bytes=" << d.span * 24
              << " widest_offset_range=" << d.widest << " widest_day=" << d.widestDay
              << " int16_headroom=" << double(INT16_MAX + 1) / double(std::max<int64_t>(1, d.widest)) << '\n';
}

// One FLD1 day re-laid as the section 5 day file: 24 hour sections, the 1h day
// chunk and the 1d piece (trades.big sections stay absent: not prototyped).
// Every hour present in FLD1 is taken as sealed and complete; an hour without
// bars is a sealed empty section.
struct LeanDay {
    proto::DayFile file;
    std::vector<flow::FlowBar> hourBars;
    std::optional<flow::FlowBar> dayBar;
    std::vector<std::vector<uint8_t>> hourPayloads;
};
LeanDay leanDay(const std::string& symbol, const flow::DayFile& day, LeanSizes* minute, LeanSizes* hour, Checks& checks) {
    LeanDay out;
    out.file.instrument = "coinbase:" + symbol;
    out.file.dayStartMs = day.dayStartMs;
    out.file.grid = {std::llround(day.nativeTick * double(day.priceScale)), day.priceScale, day.baseScale, day.quoteScale};
    LeanSizes scratch;
    for (int h = 0; h < 24; ++h) {
        const int64_t from = day.dayStartMs + h * Hour;
        std::vector<flow::FlowBar> bars;
        for (const auto& b : day.bars) if (b.startMs >= from && b.startMs < from + Hour) bars.push_back(b);
        auto payload = chunkSizes(minute ? *minute : scratch, bars, Minute, from, 60, checks);
        const auto summary = proto::encode(proto::fromBars(bars, Minute, from, 60), proto::Encoding::Spec, true);
        out.file.sections[size_t(h)] = {true, uint32_t(summary.size()), 0, payload};
        out.hourPayloads.push_back(std::move(payload));
        if (!bars.empty()) out.hourBars.push_back(flow::composeFlowBars(bars, Hour));
    }
    auto hourPayload = chunkSizes(hour ? *hour : scratch, out.hourBars, Hour, day.dayStartMs, 24, checks);
    out.file.sections[24] = {true, uint32_t(proto::encode(proto::fromBars(out.hourBars, Hour, day.dayStartMs, 24),
                                                          proto::Encoding::Spec, true).size()), 0, hourPayload};
    std::vector<flow::FlowBar> dayBars;
    if (!day.bars.empty()) {
        try { out.dayBar = flow::composeFlowBars(day.bars, Day); dayBars.push_back(*out.dayBar); }
        catch (const std::out_of_range&) {}
    }
    const auto piece = proto::fromBars(dayBars, Day, day.dayStartMs, 1);
    out.file.sections[25] = {true, uint32_t(proto::encode(piece, proto::Encoding::Spec, true).size()), 0,
                             proto::encode(piece, proto::Encoding::Spec)};
    return out;
}

void columnar(const fs::path& root, const std::string& symbol, const std::vector<int64_t>& days) {
    Checks checks;
    // Per listed day: the 24 hour chunks, the 1h day chunk and the whole day file against FLD1.
    for (const auto dayMs : days) {
        const auto bytes = readAll(dayPath(root, symbol, dayMs));
        auto start = Clock::now();
        const auto day = flow::decodeFlowDay(bytes, flow::DayCheck::BarsOnly);
        const double fldDecodeMs = msSince(start);
        LeanSizes minute, hour;
        const auto lean = leanDay(symbol, day, &minute, &hour, checks);
        const auto file = proto::encodeDayFile(lean.file);
        Size3 fldFile, leanFile;
        fldFile.add(bytes);
        leanFile.add(file);
        // Read cost: parse the day file (header, table, CRCs) and decode its 24 hour sections.
        start = Clock::now();
        const auto parsed = proto::parseDayFile(file);
        size_t decodedBars = 0;
        for (int h = 0; h < 24; ++h)
            decodedBars += proto::decode(parsed.sections[size_t(h)].payload, proto::Encoding::Spec, Minute,
                                         dayMs + h * Hour, 60).bars();
        const double leanDecodeMs = msSince(start);
        checks.dayFile += !(parsed == lean.file);
        printLean(symbol, "day=" + dayName(dayMs) + " level=1m unit=hour_chunks", minute);
        printLean(symbol, "day=" + dayName(dayMs) + " level=1h unit=day_chunk", hour);
        size_t sectionBytes = 0;
        for (const auto& s : lean.file.sections) sectionBytes += s.payload.size();
        std::cout << "mode=columnar symbol=" << symbol << " day=" << dayName(dayMs) << " unit=day_file"
                  << fmt("fld1_file", fldFile, day.bars.size()) << fmt("flc_file", leanFile, day.bars.size())
                  << " flc_header_and_table=" << file.size() - sectionBytes
                  << " flc_1h_section=" << lean.file.sections[24].payload.size()
                  << " flc_1d_piece=" << lean.file.sections[25].payload.size()
                  << " fld1_decode_bars_only_ms=" << fldDecodeMs << " flc_parse_and_decode_hours_ms=" << leanDecodeMs
                  << " decoded_bars=" << decodedBars << '\n';
    }
    // Every FLD1 day of the symbol: 1h day chunks, 16-day 1d chunks assembled
    // from day pieces, density, headroom and big-trade floors.
    Density d1m, d1h, d1d;
    std::map<int64_t, std::map<int64_t, std::vector<uint8_t>>> pieces; // 1d chunk start -> day -> piece
    std::map<int64_t, std::vector<flow::FlowBar>> dayBars;
    uint64_t composeFailures = 0;
    LeanSizes hourAll;
    // trades.big floors from FLD1 notables (top 8 sweep-collapsed events per minute):
    // events at or above each floor; a minute whose 8th notable is at or above the
    // floor may hold more, so those counts are lower bounds (truncated_minutes).
    constexpr std::array<int64_t, 6> floorsUsd{10'000, 25'000, 50'000, 100'000, 250'000, 1'000'000};
    std::array<uint64_t, 6> bigEvents{}, bigTruncated{};
    uint64_t bigDays = 0;
    for (const auto& entry : fs::directory_iterator(root / symbol / "60000")) {
        if (entry.path().extension() != ".flb") continue;
        int64_t dayMs = 0;
        try { dayMs = parseDay(entry.path().stem().string()); } catch (const std::exception&) { continue; }
        const auto name = dayName(dayMs);
        const auto day = flow::decodeFlowDay(readAll(entry.path()), flow::DayCheck::BarsOnly);
        if (day.bars.empty()) continue;
        for (const auto& b : day.bars) d1m.add(b, name);
        ++bigDays;
        for (const auto& b : day.bars)
            for (size_t f = 0; f < floorsUsd.size(); ++f) {
                const int64_t floor = floorsUsd[f] * b.quoteScale;
                for (const auto& n : b.notable) bigEvents[f] += n.notional >= floor;
                bigTruncated[f] += b.notable.size() == flow::NotableCapacity && b.notable.back().notional >= floor;
            }
        const auto lean = leanDay(symbol, day, nullptr, &hourAll, checks);
        for (const auto& b : lean.hourBars) d1h.add(b, name);
        if (!lean.dayBar) {
            ++composeFailures;
            std::cout << "mode=density symbol=" << symbol << " day=" << name << " compose_1d=int16_overflow\n";
            continue;
        }
        d1d.add(*lean.dayBar, name);
        const auto chunkStart = heatmap::chunkStartOf(Day, dayMs);
        pieces[chunkStart][dayMs] = lean.file.sections[25].payload;
        dayBars[chunkStart].push_back(*lean.dayBar);
    }
    printLean(symbol, "level=1h unit=day_chunks all_days", hourAll);
    for (auto& [start, bars] : dayBars) {
        std::sort(bars.begin(), bars.end(), [](const auto& a, const auto& b) { return a.startMs < b.startMs; });
        LeanSizes s;
        auto direct = proto::fromBars(bars, Day, start, uint32_t(heatmap::kDayChunkDays));
        // Assembly from the day pieces must give the direct encoding (absent days: unrecorded).
        std::vector<std::vector<uint8_t>> span(static_cast<size_t>(heatmap::kDayChunkDays));
        for (const auto& [day, piece] : pieces[start]) span[size_t((day - start) / Day)] = piece;
        const auto assembled = proto::assembleDayChunk(start, span, proto::Status::Unrecorded);
        direct.coverage = assembled.coverage;
        checks.assembly += !(assembled == direct);
        chunkSizes(s, bars, Day, start, uint32_t(heatmap::kDayChunkDays), checks);
        printLean(symbol, "level=1d unit=16_day_chunk start=" + dayName(start) + " days=" + std::to_string(bars.size()), s);
    }
    for (size_t f = 0; f < floorsUsd.size(); ++f)
        std::cout << "mode=bigtrades symbol=" << symbol << " floor_usd=" << floorsUsd[f] << " days=" << bigDays
                  << " events_lower_bound=" << bigEvents[f]
                  << " events_per_day=" << double(bigEvents[f]) / double(std::max<uint64_t>(1, bigDays))
                  << " truncated_minutes=" << bigTruncated[f] << '\n';
    printDensity(symbol, "1m", d1m);
    printDensity(symbol, "1h", d1h);
    printDensity(symbol, "1d", d1d);
    std::cout << "mode=columnar symbol=" << symbol << " check roundtrip_mismatch_bars=" << checks.roundTrip
              << " assembly_mismatch_chunks=" << checks.assembly << " day_file_mismatch=" << checks.dayFile
              << " lowrow_not_floor_of_low=" << checks.stats.lowRowMismatch
              << " native_tick_not_row_tick_over_scale=" << checks.stats.nativeTickMismatch
              << " compose_1d_int16_overflow_days=" << composeFailures << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 5 && std::string_view(argv[1]) == "--lateness") {
            for (int i = 4; i < argc; ++i) latenessDay(argv[2], argv[3], parseDay(argv[i]));
            return 0;
        }
        if (argc >= 5 && std::string_view(argv[1]) == "--columnar") {
            std::vector<int64_t> days;
            for (int i = 4; i < argc; ++i) days.push_back(parseDay(argv[i]));
            columnar(argv[2], argv[3], days);
            return 0;
        }
        if (argc < 4)
            throw std::invalid_argument("usage: flow_history_bench <fld1-root> <symbol> <end-ms|YYYY-MM-DDTHH:MM:SSZ> "
                                        "[--journal <rawl2-root> --day YYYY-MM-DD] [--passes N]");
        const fs::path root = argv[1];
        const std::string symbol = argv[2];
        const int64_t endMs = parseTime(argv[3]);
        std::optional<fs::path> journal;
        std::optional<int64_t> journalDayMs;
        int passes = 2;
        for (int i = 4; i + 1 < argc; i += 2) {
            const std::string flag = argv[i];
            if (flag == "--journal") journal = argv[i + 1];
            else if (flag == "--day") journalDayMs = parseDay(argv[i + 1]);
            else if (flag == "--passes") passes = std::max(1, std::stoi(argv[i + 1]));
            else throw std::invalid_argument("unknown flag " + flag);
        }
        const auto first = firstDay(root, symbol);
        if (!first) throw std::runtime_error("no FLD1 day files for " + symbol);
        std::cout << "note=measured_subset_ms is read+decode+validate_reencode+compose+encode+json+envelope. "
                     "json and envelope are a bench copy of SentinelStreamServer flow_history::buildPage/envelope "
                     "with one hourly store range per hour. Not included: FlowStore::window index stamp checks, "
                     "sourceState journal inventory, sealedThroughMs gating, the live RAM merge, history pool "
                     "queueing and the wire send. first_touch_index_ms (FlowStore::loadIndex: .state read, SHA-256, "
                     "validated decode) is paid once per day per server process: it is charged to the first page in this run that touches the day "
                     "(later pages and passes report 0) and is reported separately; "
                     "summary_ms is the footer-only pass, also outside the subset.\n";
        std::cout << "fld1_root=" << root.string() << " symbol=" << symbol << " end_ms=" << endMs
                  << " first_day=" << dayName(*first) << " page_bars=" << PageBars << '\n';
        std::set<fs::path> indexed; // one server process: each day's index loads once for the whole run
        for (const int64_t tf : {Minute, Hour, Day})
            for (int pass = 1; pass <= passes; ++pass)
                printPage(symbol, tf, pass, page(root, symbol, endMs, tf, *first, indexed));
        if (journal && journalDayMs) journalDay(*journal, root, symbol, *journalDayMs);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flow_history_bench: " << e.what() << '\n';
        return 1;
    }
}
