// Offline real-data check of stored rollup levels (not a ctest; read-only on the
// HMC2 tree). Commands, all times UTC day starts in epoch ms:
//   build  <hmc2> <out> <symbol> <firstDay> <endDay>   build closed days, per-day ms/bytes
//   oracle <hmc2> <out> <symbol> <firstDay> <endDay>   stored 1h/4h/1d vs composed minutes
//   serve  <hmc2> <out> <symbol> <endMs> <before|after> widest-view chunk plan cost
// Output: one JSON object per line on stdout.
#include "heatmap/BinCell.hpp"
#include "heatmap/HeatmapTiles.hpp"
#include "heatmap/RecordingLoader.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/RollupStore.hpp"
#include <nlohmann/json.hpp>
#include <sys/resource.h>
#include <chrono>
#include <iostream>
#include <map>
#include <numeric>
#include <set>

using namespace recording;
using namespace heatmap;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Chunks = std::vector<std::shared_ptr<const StoredChunk>>;
namespace {
double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }
long peakRssMb() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_maxrss / (1024 * 1024); }
void emit(const nlohmann::json& j) { std::cout << j.dump() << std::endl; }
std::shared_ptr<const StoredChunk> stored(SparseColumns columns) {
    static uint64_t generation = 0;
    auto chunk = std::make_shared<StoredChunk>();
    chunk->columns = std::make_shared<SparseColumns>(std::move(columns));
    chunk->generation = ++generation;
    return chunk;
}
RollupStore::Options options(const fs::path& hmc2, const fs::path& out, const std::string& symbol, int64_t through) {
    RollupStore::Options o;
    o.hmc2Root = hmc2;
    o.root = out;
    o.symbols = {symbol};
    o.minuteThrough = [through](const std::string&, const std::string&) { return through; };
    return o;
}

int build(const fs::path& hmc2, const fs::path& out, const std::string& symbol, int64_t first, int64_t end) {
    // Per-day timing with the builder's own functions (no writes), then the
    // store's pass writes the files. Both layers.
    Hmc2Reader reader(hmc2);
    for (const std::string layer : {"near", "deep"})
        for (int64_t d = first; d < end; d += kDayMs) {
            const auto started = Clock::now();
            ReadControl control;
            const auto rollup = composeDay(reader, symbol, layer, d, control);
            const double composeMs = ms(started);
            const auto hour = encodeHourFrame(symbol, rollup, d), piece = encodeDayPiece(symbol, rollup, d);
            emit({{"phase", "build_day"}, {"layer", layer}, {"day", d}, {"compose_ms", composeMs},
                  {"total_ms", ms(started)}, {"hour_bytes", hour.size() + kRollupHeaderBytes},
                  {"day_bytes", piece.size() + kRollupHeaderBytes}, {"hour_columns", rollup.hour.columns.size()},
                  {"peak_rss_mb", peakRssMb()}});
        }
    RollupStore store(options(hmc2, out, symbol, end));
    const auto started = Clock::now();
    const auto pass = store.runPass();
    emit({{"phase", "build_pass"}, {"built", pass.built}, {"failed", pass.failed}, {"pending", pass.pending},
          {"ms", ms(started)}, {"peak_rss_mb", peakRssMb()}});
    return pass.failed ? 2 : 0;
}

uint32_t word(const BinCell& c) {
    return tiles::cellWord(c.valid ? tiles::kCellValid : tiles::kCellVeil, c.code, c.dominantAsk);
}
struct Diff { uint64_t compared = 0, cells = 0, states = 0; };
void compareCells(const SparseColumns& expected, const SparseColumns& actual, int multiple, Diff& diff) {
    for (int64_t t = expected.startMs; t < expected.endMs; t += expected.tfMs) {
        if (bucketState(expected, t) != bucketState(actual, t)) { ++diff.states; continue; }
        if (bucketState(expected, t) != BucketState::Present) continue;
        auto find = [&](const SparseColumns& s) -> const SparseColumn& {
            return *std::lower_bound(s.columns.begin(), s.columns.end(), t,
                                     [](const SparseColumn& c, int64_t v) { return c.bucketStartMs < v; });
        };
        const auto& a = find(expected);
        const auto& b = find(actual);
        int64_t lcm = 0;
        for (const auto& n : a.native) lcm = lcm ? std::lcm(lcm, n.grid.rowTickUnits) : n.grid.rowTickUnits;
        const int64_t tickUnits = lcm * multiple;
        const double tick = double(tickUnits) / 100.0;
        auto extent = tiles::usefulUnits(expected, 100, t, t + expected.tfMs);
        const auto other = tiles::usefulUnits(actual, 100, t, t + expected.tfMs);
        extent.lo = std::min(extent.lo, other.lo);
        extent.end = std::max(extent.end, other.end);
        const double lo = std::max(0.0, std::floor(double(extent.lo) / double(tickUnits)) * tick - tick);
        const double hi = std::ceil(double(extent.end) / double(tickUnits)) * tick + tick;
        for (double bandHi = hi; bandHi > lo;) { // binColumn bins at most 16384 rows per call
            const double bandLo = std::max(lo, bandHi - 8192 * tick);
            const auto x = binColumn(a, bandLo, bandHi, tick), y = binColumn(b, bandLo, bandHi, tick);
            for (size_t i = 0; i < x.size(); ++i) {
                ++diff.compared;
                if (i >= y.size() || word(x[i]) != word(y[i])) ++diff.cells;
            }
            bandHi = bandLo;
        }
    }
}
int oracle(const fs::path& hmc2, const fs::path& out, const std::string& symbol, int64_t first, int64_t end) {
    RollupStore store(options(hmc2, out, symbol, end));
    store.runPass(); // indexes the built files (builds nothing new when current)
    Hmc2Reader reader(hmc2);
    std::map<std::string, Diff> diffs;
    uint64_t fallbacks = 0;
    for (const std::string layer : {"near", "deep"}) {
        const std::string source = "hmc2." + layer;
        for (int64_t d = first; d < end; d += kDayMs) {
            Chunks minutes;
            for (int64_t h = d; h < d + kDayMs; h += kHourMs)
                minutes.push_back(stored(buildChunk(reader, {symbol, source, kMinuteMs, h})));
            const int64_t span = chunkSpanMs(kDayMs);
            Chunks hour, day;
            for (const auto& [key, into] : {std::pair{ChunkKey{symbol, source, kHourMs, d}, &hour},
                                            std::pair{ChunkKey{symbol, source, kDayMs, chunkStartOf(kDayMs, d)}, &day}}) {
                const auto reply = store.serve(key, reader);
                if (!reply.wire) throw std::runtime_error("serve failed: " + reply.message);
                fallbacks += reply.fallback;
                into->push_back(stored(decodeChunk(*reply.wire).columns));
            }
            for (const int64_t tf : {kHourMs, 4 * kHourMs, kDayMs}) {
                const auto expected = tiles::composeChunks(minutes, symbol, layer, tf, d, d + kDayMs);
                const auto actual = tiles::composeChunks(tf == kDayMs ? day : hour, symbol, layer, tf, d, d + kDayMs);
                for (const int multiple : {1, 5, 50})
                    compareCells(expected, actual, multiple,
                                 diffs[layer + "/" + std::to_string(tf) + "/x" + std::to_string(multiple)]);
            }
            std::cerr << "oracle " << layer << ' ' << d << '\n';
        }
    }
    uint64_t cells = 0, differences = 0;
    for (const auto& [name, d] : diffs) {
        emit({{"phase", "oracle"}, {"case", name}, {"compared", d.compared}, {"cell_differences", d.cells},
              {"state_differences", d.states}});
        cells += d.compared;
        differences += d.cells + d.states;
    }
    emit({{"phase", "oracle_summary"}, {"compared", cells}, {"differences", differences}, {"fallbacks", fallbacks}});
    return differences ? 2 : 0;
}

// The chunk plan of the widest displayable view (1500 buckets or the available
// history) at tf, streamed tile by tile as the GUI builds spans: every chunk a
// tile needs is served (or built), decoded and composed; chunks shared by two
// tiles are counted once.
struct Cost { uint64_t chunks = 0, wireBytes = 0, decodedBytes = 0; double fetchMs = 0, composeMs = 0; };
int serve(const fs::path& hmc2, const fs::path& out, const std::string& symbol, int64_t endMs, bool after) {
    Hmc2Reader reader(hmc2);
    std::unique_ptr<RollupStore> store;
    if (after) {
        store = std::make_unique<RollupStore>(options(hmc2, out, symbol, endMs));
        store->runPass();
    }
    for (const int64_t tf : {kHourMs, 4 * kHourMs, kDayMs}) {
        Cost cost;
        for (const std::string layer : {"near", "deep"}) {
            const std::string source = "hmc2." + layer;
            ReadControl control;
            const auto minutes = reader.availability(symbol, layer, kMinuteMs, control);
            tiles::Availability a;
            a.oldestMs = *minutes.oldestMs;
            a.minuteOldestMs = *minutes.oldestMs;
            a.endMs = endMs;
            if (after) {
                for (const auto& level : store->levels(symbol, layer))
                    tiles::addLevel(a, level.levelMs, level.oldestMs, level.throughMs);
            } else if (layer == "deep") { // main: the recorder's deep-3600000 hour files, one UTC day per chunk
                ReadControl hourControl;
                const auto hours = reader.availability(symbol, layer, kHourMs, hourControl);
                a.oldestMs = std::min(a.oldestMs, *hours.oldestMs);
                a.hourOldestMs = *hours.oldestMs;
                a.hourThroughMs = std::min(endMs, *hours.latestMs + kHourMs);
            }
            const int64_t end = endMs / tf * tf;
            const int64_t start = std::max(end - 1500 * tf, a.oldestMs / tf * tf);
            std::set<std::pair<int64_t, int64_t>> seen;
            for (int64_t tile = tiles::tileOfBucket(start / tf); tiles::tileStartMs(tile, tf) < end; ++tile) {
                const int64_t ts = std::max(tiles::tileStartMs(tile, tf), start);
                const int64_t te = std::min(tiles::tileEndMs(tile, tf), end);
                Chunks chunks;
                for (const auto& key : tiles::chunksFor(symbol, source, tf, ts, te, a)) {
                    const bool counted = seen.insert({key.levelMs, key.startMs}).second;
                    const auto started = Clock::now();
                    std::vector<uint8_t> wire;
                    if (key.levelMs == kMinuteMs) {
                        wire = encodeChunk({ChunkKind::Chunk, key, {true, key.startMs + kHourMs, 0}, buildChunk(reader, key)});
                    } else if (after) {
                        const auto reply = store->serve(key, reader);
                        if (!reply.wire) throw std::runtime_error("serve failed: " + reply.message);
                        wire = *reply.wire;
                    } else { // main's deep hour chunk: the recorder hour records of one UTC day
                        SparseColumns columns{symbol, layer, kHourMs, key.startMs, key.startMs + kDayMs};
                        ReadControl hourControl;
                        const auto scan = reader.visit(symbol, layer, kHourMs, key.startMs, key.startMs + kDayMs,
                            [&](const Hmc2Record& r) { if (r.observedMs) columns.columns.push_back(fromRecording(r)); },
                            hourControl);
                        columns.scannedRanges.push_back({scan.scannedStartMs, scan.scannedEndMs});
                        wire = encodeChunk({ChunkKind::Chunk, key, {true, key.startMs + kDayMs, 0}, columns});
                    }
                    auto decoded = decodeChunk(wire);
                    const double fetch = ms(started);
                    if (counted) {
                        ++cost.chunks;
                        cost.wireBytes += wire.size();
                        cost.decodedBytes += sparseBytes(decoded.columns);
                        cost.fetchMs += fetch;
                    }
                    chunks.push_back(stored(std::move(decoded.columns)));
                }
                const auto started = Clock::now();
                (void)tiles::composeChunks(chunks, symbol, layer, tf, tiles::tileStartMs(tile, tf), tiles::tileEndMs(tile, tf));
                cost.composeMs += ms(started);
            }
        }
        emit({{"phase", "serve"}, {"mode", after ? "after" : "before"}, {"tf_ms", tf}, {"chunks", cost.chunks},
              {"wire_bytes", cost.wireBytes}, {"decoded_bytes", cost.decodedBytes}, {"fetch_decode_ms", cost.fetchMs},
              {"compose_ms", cost.composeMs}, {"peak_rss_mb", peakRssMb()}});
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::cerr << "exe=" << fs::absolute(argv[0]) << " built=" << __DATE__ << ' ' << __TIME__ << '\n';
        if (argc < 7) throw std::invalid_argument("usage: build|oracle <hmc2> <out> <symbol> <firstDay> <endDay> | serve <hmc2> <out> <symbol> <endMs> before|after");
        const std::string command = argv[1];
        const fs::path hmc2 = argv[2], out = argv[3];
        const std::string symbol = argv[4];
        if (command == "build") return build(hmc2, out, symbol, std::stoll(argv[5]), std::stoll(argv[6]));
        if (command == "oracle") return oracle(hmc2, out, symbol, std::stoll(argv[5]), std::stoll(argv[6]));
        if (command == "serve") return serve(hmc2, out, symbol, std::stoll(argv[5]), std::string(argv[6]) == "after");
        throw std::invalid_argument("unknown command");
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
