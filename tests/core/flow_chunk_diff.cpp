// Offline FLC1 migration evidence. RAWL2/HMC2/FLD1 are read-only; every generated
// file goes under an explicit scratch root, checked disjoint from all sources.
// Usage: flow_chunk_diff <raw-root> <hmc-root> <fld-root> <scratch> <symbol> <YYYY-MM-DD>...|--steady
// FLD1 has no trade IDs. A legacy day replay must reproduce each differing FLD1
// bar before its trade-ID membership is asserted; otherwise evidence is flagged
// unresolved and the command fails. Notables are excluded from comparisons.
#include "servermodel/FlowChunkStore.hpp"
#include "flow/FlowChunkCodec.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "roller/JournalFeed.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <fstream>
#include <iostream>
#include <set>
namespace fs = std::filesystem;
namespace flow = sentinel::flow;
namespace c = flow::chunks;
namespace roller = sentinel::roller;
constexpr int64_t Minute = 60000, Hour = 60 * Minute, Day = 24 * Hour;
std::vector<uint8_t> readFile(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
flow::FlowBar stripped(flow::FlowBar b) { b.notable.clear(); return b; }
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc < 7) throw std::invalid_argument("usage: flow_chunk_diff RAW HMC FLD SCRATCH SYMBOL YYYY-MM-DD...");
        const fs::path raw = argv[1], hmc = argv[2], fld = argv[3], scratch = fs::absolute(argv[4]);
        const std::string symbol = argv[5], id = "coinbase:" + symbol;
        // The scratch tree itself, not merely its output child, must be disjoint.
        if (recording::flowRootFor(scratch.string(), hmc, raw, {}, {fld}).empty())
            throw std::invalid_argument("scratch overlaps source data");
        recording::FlowStore::Options sources;
        sources.journalRoot = raw; sources.hmc2Root = hmc; sources.root = scratch / "unused-fld";
        sources.symbols = {symbol}; sources.protectedRoots = {fld}; sources.hourThrough = [](const std::string &) { return INT64_MAX / Day * Day; };
        recording::FlowStore archiveOwner(sources); // never starts/builds FLD1
        recording::FlowChunkStore::Options options; options.root = scratch / "flc"; options.lowerPriority = false;
        size_t inventoryRefreshes = 0;
        options.afterArchiveRefreshForTest = [&] { ++inventoryRefreshes; };
        recording::FlowChunkStore store(archiveOwner, options);
        if (std::string_view(argv[6]) == "--steady") {
            const auto first = store.runPass();
            if (first.failed || first.stopped) throw std::runtime_error("initial backfill failed");
            inventoryRefreshes = 0;
            const auto start = std::chrono::steady_clock::now();
            const auto second = store.runPass();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (second.failed || second.stopped) throw std::runtime_error("steady pass failed");
            size_t days = 0;
            for (const auto &entry : fs::directory_iterator(scratch / "flc" / "coinbase" / symbol))
                if (entry.path().extension() == ".flc") ++days;
            std::cout << "instrument=" << id << " sealed_days=" << days << " steady_inventory_refreshes="
                      << inventoryRefreshes << " steady_pass_ms=" << elapsed << '\n';
            return 0;
        }
        bool unresolved = false;
        for (int a = 6; a < argc; ++a) {
            const auto date = QDate::fromString(argv[a], "yyyy-MM-dd");
            if (!date.isValid()) throw std::invalid_argument("invalid date");
            const auto day = date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch();
            const auto begin = std::chrono::steady_clock::now();
            const auto result = store.rebuildDay(id, day);
            const auto path = store.filePath(id, day);
            const auto encoded = readFile(path);
            const auto built = c::parseDayFile(encoded);
            if (result.failed || !std::all_of(built.sections.begin(), built.sections.begin() + 24,
                [](const auto &section) { return section.sealed; }))
                throw std::runtime_error("day not fully sealable");
            const auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            const auto legacy = flow::decodeFlowDay(readFile(fld / symbol / "60000" / (std::string(argv[a]) + ".flb")));
            std::map<int64_t, flow::FlowBar> before, after;
            for (const auto &bar : legacy.bars) before.emplace(bar.startMs, stripped(bar));
            for (int h = 0; h < 24; ++h) {
                const auto &s = built.sections[h];
                for (auto &bar : c::toBars(c::decode(s.payload, Minute, day + h * Hour, 60, false, uint64_t(1) << s.stepExponent)))
                    after.emplace(bar.startMs, std::move(bar));
            }
            std::set<int64_t> differences;
            for (const auto &[ms, bar] : before) if (!after.contains(ms) || after.at(ms) != bar) differences.insert(ms);
            for (const auto &[ms, bar] : after) if (!before.contains(ms) || before.at(ms) != bar) differences.insert(ms);
            size_t moved = 0;
            if (!differences.empty()) {
                const auto archive = archiveOwner.archive(symbol);
                const auto grid = archiveOwner.gridFor(symbol, day, *archive.catalog);
                std::map<std::string, flow::Trade> canonical, oldTrades;
                for (int h = 0; h < 24; ++h) {
                    if (!std::any_of(differences.begin(), differences.end(), [&](auto ms) { return ms / Hour == (day + h * Hour) / Hour; })) continue;
                    const auto scan = store.buildHour(symbol, day + h * Hour, grid, *archive.catalog, {}, true);
                    for (const auto &t : scan.accepted) if (differences.contains(t.exchangeMs / Minute * Minute)) canonical.emplace(t.id, t);
                }
                // Same whole-day run as an initially empty FLD1 build.
                size_t chosen = 0;
                for (size_t i = 0; i < archive.catalog->files().size(); ++i)
                    if (archive.catalog->files()[i].openedNs / 1000000 <= day - Minute) chosen = i;
                const auto &file = archive.catalog->files()[chosen];
                roller::JournalReader reader(*archive.catalog, roller::JournalPos{symbol, file.run, file.firstBlock, 0});
                roller::JournalFeed feed(symbol);
                const auto &s = grid.scales;
                flow::FlowBarBuilder builder(Minute, s.nativeTick, s.priceScale, s.baseScale, s.quoteScale);
                feed.onTrade = [&](const ::Trade &v) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(v.timestamp.time_since_epoch()).count();
                    if (!differences.contains(ms / Minute * Minute)) return;
                    flow::Trade t{v.trade_id, ms, v.price, v.size, v.side == AggressorSide::Buy ? flow::Side::Buy : v.side == AggressorSide::Sell ? flow::Side::Sell : flow::Side::Unknown};
                    if (builder.add(t) == flow::FlowBarBuilder::AddResult::Accepted) oldTrades.emplace(t.id, t);
                };
                roller::JournalRecord record;
                while (reader.next(record)) {
                    if (record.record.time.systemNs / 1000000 > day + Day + Minute) break;
                    feed.apply(record);
                }
                std::map<int64_t, flow::FlowBar> reconstructed;
                for (const auto &b : builder.bars()) reconstructed.emplace(b.startMs, stripped(b));
                for (auto ms : differences) {
                    const bool proven = before.contains(ms) == reconstructed.contains(ms) &&
                        (!before.contains(ms) || before.at(ms) == reconstructed.at(ms));
                    unresolved |= !proven;
                    std::cout << "bar=" << ms << " legacy_membership_proven=" << proven << '\n';
                    for (const auto &[tid, t] : oldTrades) if (t.exchangeMs / Minute * Minute == ms && !canonical.contains(tid)) {
                        ++moved; std::cout << " moved=excluded id=" << tid << " exchange_ms=" << t.exchangeMs << " price=" << t.price << " size=" << t.size << '\n';
                    }
                    for (const auto &[tid, t] : canonical) if (t.exchangeMs / Minute * Minute == ms && !oldTrades.contains(tid)) {
                        ++moved; std::cout << " moved=included id=" << tid << " exchange_ms=" << t.exchangeMs << " price=" << t.price << " size=" << t.size << '\n';
                    }
                }
            }
            std::cout << "instrument=" << id << " day=" << argv[a] << " bars=" << after.size()
                << " differing_bars=" << differences.size() << " trades_moved=" << moved
                << " late_trades=" << built.lateTrades << " regressions=" << built.regressions
                << " build_ms=" << buildMs << " flc_bytes=" << encoded.size() << " unresolved=" << unresolved << '\n';
        }
        return unresolved ? 2 : 0;
    } catch (const std::exception &e) { std::cerr << "flow_chunk_diff: " << e.what() << '\n'; return 1; }
}
