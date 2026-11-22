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
// day file the page touches is read whole, as the server does. Pass 1 may hit a
// cold OS cache; later passes are warm.
// Journal mode (--journal, --day): one UTC day of RAWL2 replayed through
// JournalFeed (every frame, FM-278) for its trades, against reading and decoding
// that day's FLD1 file.
#include "flow/FlowCodec.hpp"
#include "flow/FlowCompose.hpp"
#include "roller/JournalFeed.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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
    int days = 0, missingDays = 0;
    size_t bytesRead = 0, barsIn = 0, barsOut = 0, rowsOut = 0, encodedBytes = 0, summaryBytes = 0;
    double readMs = 0, decodeMs = 0, validateMs = 0, composeMs = 0, encodeMs = 0, summaryMs = 0, totalMs = 0;
};
struct Summary { int64_t buy = 0, sell = 0, delta = 0, base = 0; };

// One UTC day of the page, the way SentinelStreamServer::flow_history::build
// slices a page by day and buildPage composes each bucket.
void pageDay(PageStats& s, const fs::path& path, int64_t from, int64_t to, int64_t tf) {
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

    std::map<int64_t, std::vector<flow::FlowBar>> buckets;
    for (auto& bar : day.bars)
        if (bar.startMs >= from && bar.startMs < to) buckets[bar.startMs / tf * tf].push_back(std::move(bar));
    for (const auto& [_, parts] : buckets) s.barsIn += parts.size();

    start = Clock::now();
    std::map<int64_t, Summary> summaries;
    for (const auto& [bucket, parts] : buckets) {
        auto& sum = summaries[bucket];
        for (const auto& bar : parts) {
            sum.buy += bar.buyVol; sum.sell += bar.sellVol; sum.delta += bar.delta; sum.base += bar.baseVolume;
        }
    }
    s.summaryMs += msSince(start);
    s.summaryBytes += summaries.size() * SummaryBytesPerBar;

    start = Clock::now();
    std::vector<flow::FlowBar> composed;
    composed.reserve(buckets.size());
    for (auto& [_, parts] : buckets)
        composed.push_back(tf == parts.front().timeframeMs ? std::move(parts.front()) : flow::composeFlowBars(parts, tf));
    s.composeMs += msSince(start);

    start = Clock::now();
    for (const auto& bar : composed) {
        s.encodedBytes += 4 + flow::encodeFlowBar(bar).size(); // u32 length prefix per frame
        s.rowsOut += bar.rows.size();
    }
    s.encodeMs += msSince(start);
    s.barsOut += composed.size();
}

PageStats page(const fs::path& root, const std::string& symbol, int64_t endMs, int64_t tf, int64_t firstDayMs) {
    PageStats s;
    s.toMs = endMs / tf * tf;
    s.fromMs = std::max(s.toMs - PageBars * tf, firstDayMs / tf * tf);
    const auto start = Clock::now();
    for (int64_t day = s.fromMs / Day * Day; day < s.toMs; day += Day) {
        const auto path = dayPath(root, symbol, day);
        if (!fs::exists(path)) { ++s.missingDays; continue; }
        ++s.days;
        pageDay(s, path, std::max(s.fromMs, day), std::min(s.toMs, day + Day), tf);
    }
    s.totalMs = msSince(start);
    return s;
}

void printPage(const std::string& symbol, int64_t tf, int pass, const PageStats& s) {
    std::cout << "mode=page symbol=" << symbol << " tf_ms=" << tf << " pass=" << pass
              << " from_ms=" << s.fromMs << " to_ms=" << s.toMs
              << " day_files=" << s.days << " missing_days=" << s.missingDays
              << " bytes_read=" << s.bytesRead << " read_ms=" << s.readMs
              << " decode_ms=" << s.decodeMs << " validate_reencode_ms=" << s.validateMs
              << " compose_ms=" << s.composeMs << " encode_ms=" << s.encodeMs << " total_ms=" << s.totalMs
              << " bars_in=" << s.barsIn << " bars_out=" << s.barsOut << " rows_out=" << s.rowsOut
              << " encoded_bytes=" << s.encodedBytes
              << " summary_bytes=" << s.summaryBytes << " summary_ms=" << s.summaryMs
              << " summary_share=" << (s.encodedBytes ? double(s.summaryBytes) / double(s.encodedBytes) : 0.0) << '\n';
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
} // namespace

int main(int argc, char** argv) {
    try {
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
        std::cout << "fld1_root=" << root.string() << " symbol=" << symbol << " end_ms=" << endMs
                  << " first_day=" << dayName(*first) << " page_bars=" << PageBars << '\n';
        for (const int64_t tf : {Minute, Hour, Day})
            for (int pass = 1; pass <= passes; ++pass) printPage(symbol, tf, pass, page(root, symbol, endMs, tf, *first));
        if (journal && journalDayMs) journalDay(*journal, root, symbol, *journalDayMs);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flow_history_bench: " << e.what() << '\n';
        return 1;
    }
}
