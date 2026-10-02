// Read-only local-transport benchmark; no server, GUI, settings or writer lock.
// Cold = decoded chunks held, composed-window cache empty. Warm = re-bin and
// format from that cache (not reuse of a finished LabelCells). Wall-clock ms.
#include "render/heatmap/HeatmapCellQuery.hpp"
#include "heatmap/LocalChunkTransport.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <chrono>
#include <iostream>
#include <set>

using namespace heatmap;
template<class Predicate> void waitFor(Predicate ready) {
    QElapsedTimer timer; timer.start();
    while (!ready()) {
        if (timer.elapsed() > 120000) throw std::runtime_error("local transport timed out");
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::cerr << "usage: heatmap_cell_query_bench RECORDING_ROOT [columns=120] [rows=100]\n"; return 2; }
    try {
        const auto columns = argc > 2 ? uint32_t(std::stoul(argv[2])) : 120u;
        const auto rows = argc > 3 ? uint32_t(std::stoul(argv[3])) : 100u;
        ChunkStore store(512ull << 20);
        LocalChunkTransport::TestHooks hooks; hooks.pollIntervalMs = 0;
        LocalChunkTransport transport(argv[1], hooks);
        ChunkFetcher fetcher(store, transport);
        std::string error;
        QObject::connect(&fetcher, &ChunkFetcher::chunkFailed, &app, [&](auto, auto code, auto message) {
            error = code.toStdString() + ": " + message.toStdString();
        }, Qt::QueuedConnection);
        transport.start({"BTC-USD"});
        waitFor([&] { return fetcher.availability("BTC-USD").has_value(); });
        const auto availability = *fetcher.availability("BTC-USD");
        std::vector<SourceAvailability> sources;
        int64_t pin = INT64_MAX;
        for (const auto& s : availability.sources) {
            SourceAvailability a{s.id, {}};
            a.time.oldestMs = INT64_MAX;
            for (const auto& l : s.levels) {
                a.time.oldestMs = std::min(a.time.oldestMs, l.oldestMs);
                a.time.endMs = std::max(a.time.endMs, l.committedThroughMs);
                if (l.levelMs == kMinuteMs) a.time.minuteOldestMs = l.oldestMs;
                if (l.levelMs == kHourMs) { a.time.hourOldestMs = l.oldestMs; a.time.hourThroughMs = l.committedThroughMs; }
            }
            if (!a.time.minuteOldestMs) a.time.minuteOldestMs = INT64_MAX;
            pin = std::min(pin, a.time.endMs);
            sources.push_back(a);
        }
        pin = recording::floorDiv(pin, kHourMs) * kHourMs; // immutable completed hour, all cases same end
        std::cout << "pin=" << pin << " columns=" << columns << " rows=" << rows << " samples=41 cold=empty-compose-cache warm=rebin+format\n";
        bool passed = true;
        for (const int64_t tf : {kMinuteMs, 5 * kMinuteMs, kHourMs}) {
            fetcher.release(1);
            store.clear();
            const auto from = pin - columns * tf;
            auto plans = planSpans("BTC-USD", tf, double(from), double(pin), sources);
            auto less = [](const ChunkKey& a, const ChunkKey& b) {
                return std::tie(a.symbol, a.source, a.levelMs, a.startMs) < std::tie(b.symbol, b.source, b.levelMs, b.startMs);
            };
            std::set<ChunkKey, decltype(less)> keys(less);
            for (const auto& p : plans) if (p.rank.tier == SpanTier::Visible)
                for (const auto& s : p.sources) for (const auto& k : s.chunks)
                    if (k.startMs < pin && k.startMs + chunkSpanMs(k.source, k.levelMs) > from) keys.insert(k);
            fetcher.want(1, {keys.begin(), keys.end()}, 1);
            waitFor([&] {
                if (!error.empty()) throw std::runtime_error(error);
                return std::all_of(keys.begin(), keys.end(), [&](const auto& k) { return store.contains(k); });
            });
            SpanSet picture; picture.symbol = "BTC-USD"; picture.tfMs = tf; picture.version = 1;
            picture.availableStartMs = INT64_MAX; picture.availableEndMs = pin;
            for (const auto& a : sources) picture.availableStartMs = std::min(picture.availableStartMs, a.time.oldestMs);
            for (const auto& p : plans) if (p.rank.tier == SpanTier::Visible) {
                SpanSnapshot span{p.id, p.rank, {}, true};
                for (const auto& s : p.sources) {
                    auto build = std::make_shared<SpanSourceBuild>();
                    build->key = {p.id, s.source, s.availableStartMs, std::min(pin, s.availableEndMs), 100, 0, {}};
                    std::vector<std::shared_ptr<const StoredChunk>> chunks;
                    for (const auto& k : s.chunks) if (auto chunk = store.peek(k)) {
                        build->key.generations.push_back({k.symbol, k.source, k.levelMs, k.startMs, chunk->generation, chunk->sealed});
                        chunks.push_back(chunk);
                    }
                    std::sort(build->key.generations.begin(), build->key.generations.end());
                    build->commonUnits = tiles::commonUnitsIn(chunks, from, pin, tf);
                    build->completeEndMs = recording::floorDiv(build->key.availableEndMs, tf) * tf;
                    span.sources.push_back({s.source, build});
                }
                std::stable_sort(span.sources.begin(), span.sources.end(), [](const auto& a, const auto& b) {
                    return a.build->commonUnits > b.build->commonUnits;
                });
                picture.spans.push_back(std::move(span));
            }
            // Locate the observed book midpoint in the newest held minute.
            double mid = 0;
            for (auto it = keys.rbegin(); it != keys.rend() && !mid; ++it) {
                const auto chunk = store.peek(*it);
                if (chunk->columns->columns.empty()) continue;
                const auto& c = chunk->columns->columns.back();
                for (const auto& n : c.native) {
                    double bid = 0, ask = INFINITY;
                    for (const auto& e : n.entries) if (e.code) {
                        const double price = (n.baseRow + e.row()) * double(n.grid.rowTickUnits) / n.grid.priceScale;
                        if (e.isAsk()) ask = std::min(ask, price); else bid = std::max(bid, price);
                    }
                    if (bid && std::isfinite(ask)) { mid = (bid + ask) / 2; break; }
                }
            }
            if (!mid) throw std::runtime_error("recording has no midpoint");
            for (const int64_t units : {100, 500, 1000}) {
                LabelRequest q{1, picture.version, 0, tf, units, 100, from / tf,
                               int64_t(std::floor(mid / (units / 100.0))) - int64_t(rows / 2), columns, rows, "BTC"};
                LabelWindowBuilder builder;
                auto measure = [&] {
                    const auto start = std::chrono::steady_clock::now();
                    auto result = builder.build(q, picture, nullptr, store);
                    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    if (!result->missing.empty()) throw std::runtime_error("unexpected missing generation");
                    return std::pair(elapsed, result);
                };
                auto [cold, result] = measure();
                std::vector<double> warm;
                for (int i = 0; i < 41; ++i) { ++q.serial; warm.push_back(measure().first); }
                std::sort(warm.begin(), warm.end());
                const auto p95 = warm[size_t(std::ceil(0.95 * warm.size())) - 1];
                size_t valid = 0, labelled = 0;
                for (const auto& c : result->cells) {
                    valid += tiles::cellState(c.word) == tiles::kCellValid;
                    labelled += c.usd[0] != 0;
                }
                if (!labelled) throw std::runtime_error("benchmark window has no nonzero labels");
                std::cout << "tf_ms=" << tf << " tick=" << units / 100.0 << " cold_ms=" << cold
                          << " warm_p95_ms=" << p95 << " valid=" << valid << " labelled=" << labelled << " cells=" << result->cells.size()
                          << " composed=" << result->composedWindows << " cache_bytes=" << builder.bytes()
                          << " chunk_bytes=" << store.stats().bytes << std::endl;
                passed &= cold <= 50 && p95 <= 5;
            }
        }
        fetcher.release(1);
        return passed ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
