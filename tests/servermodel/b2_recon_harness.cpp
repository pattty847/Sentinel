// Throwaway B2 reconstruction measurement. Never lands.
// Usage: b2_recon_harness <root> <windowEndMs> <mode> [deliverFromMs]
//   mode: seq | par | cmp | prefilter
#include "roller/JournalFeed.hpp"
#include "roller/JournalReader.hpp"
#include "servermodel/TimeframeAggregator.hpp"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <sys/resource.h>
#include <thread>

using namespace sentinel::roller;
using Clock = std::chrono::steady_clock;

namespace {
const std::vector<std::string> kProducts{"BTC-USD", "ETH-USD", "SOL-USD", "FARTCOIN-USD",
                                         "PEPE-USD", "DOGE-USD", "AVAX-USD"};
double threadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Result {
    std::string product;
    uint64_t records = 0, recordsInWindow = 0, trades = 0, late100 = 0, late1000 = 0;
    uint64_t lagGt1s = 0, closes = 0, invalids = 0, seqFrames = 0;
    int version = 0;
    double wallMs = 0, cpuMs = 0, decodeMs = 0, feedMs = 0, aggMs = 0;
    std::vector<OHLCVBar> bars100, bars1000;
};

int64_t bucket(int64_t t, int64_t tf) { return (t / tf) * tf; }

Result run(const std::string& root, const std::string& product, int64_t deliverFrom,
           int64_t endMs, bool prefilter) {
    Result res;
    res.product = product;
    const auto t0 = Clock::now();
    const double c0 = threadCpuMs();
    JournalReader reader(root, product);
    JournalFeed feed(product);
    TimeframeAggregator agg({100, 1000, 60000});
    agg.setJournalClock(product);
    QObject::connect(&agg, &TimeframeAggregator::barClosed, [&](auto&&...) { ++res.closes; });
    bool deliver = false;
    feed.onInvalid = [&](int64_t, const std::string&) { ++res.invalids; };
    int64_t sentSecond = INT64_MIN, currentLocal = 0;
    int64_t open100 = INT64_MIN, open1000 = INT64_MIN;
    Clock::duration aggDur{};
    feed.onTrade = [&](const Trade& t) {
        if (!deliver) return;
        ++res.trades;
        const int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            t.timestamp.time_since_epoch()).count();
        if (currentLocal - ts > 1000) ++res.lagGt1s;
        if (bucket(ts, 100) < open100) ++res.late100;
        if (bucket(ts, 1000) < open1000) ++res.late1000;
        open100 = std::max(open100, bucket(ts, 100));
        open1000 = std::max(open1000, bucket(ts, 1000));
        const auto a = Clock::now();
        agg.onTrade(t);
        aggDur += Clock::now() - a;
    };
    Clock::duration decodeDur{}, feedDur{};
    JournalRecord r;
    for (;;) {
        const auto d0 = Clock::now();
        const bool ok = reader.next(r);
        decodeDur += Clock::now() - d0;
        if (!ok) break;
        ++res.records;
        res.version = r.version;
        if (r.record.payload.find("sequence_num") != std::string::npos) ++res.seqFrames;
        const int64_t local = r.record.time.systemNs / 1'000'000;
        if (local >= endMs) break;
        currentLocal = local;
        deliver = local >= deliverFrom;
        if (deliver) ++res.recordsInWindow;
        if (prefilter && r.record.kind == sentinel::capture::Kind::Frame &&
            r.record.payload.find("market_trades") == std::string::npos) {
            // skip L2 frames entirely (trade-only pass estimate)
        } else {
            const auto f0 = Clock::now();
            feed.apply(r);
            feedDur += Clock::now() - f0;
        }
        if (deliver) {
            const int64_t second = local / 1000;
            if (second > sentSecond) {
                sentSecond = second;
                const auto a = Clock::now();
                agg.advance(product, local - 1000);
                aggDur += Clock::now() - a;
                open100 = std::max(open100, bucket(local - 1000, 100));
                open1000 = std::max(open1000, bucket(local - 1000, 1000));
            }
        }
    }
    res.bars100 = agg.getHistory(product, 100, 1'000'000);
    res.bars1000 = agg.getHistory(product, 1000, 1'000'000);
    res.wallMs = ms(Clock::now() - t0);
    res.cpuMs = threadCpuMs() - c0;
    res.decodeMs = ms(decodeDur);
    res.aggMs = ms(aggDur);
    res.feedMs = ms(feedDur) - res.aggMs; // feed.apply includes onTrade -> agg
    return res;
}

void print(const Result& r) {
    std::printf("%-13s v=%d records=%llu inWindow=%llu trades=%llu closes=%llu bars100=%zu bars1s=%zu "
                "late100=%llu late1s=%llu tradeLag>1s=%llu wall=%.0fms cpu=%.0fms decode=%.0fms "
                "feed=%.0fms agg=%.1fms invalids=%llu seqFrames=%llu\n",
                r.product.c_str(), r.version, (unsigned long long)r.records,
                (unsigned long long)r.recordsInWindow, (unsigned long long)r.trades,
                (unsigned long long)r.closes, r.bars100.size(), r.bars1000.size(),
                (unsigned long long)r.late100, (unsigned long long)r.late1000,
                (unsigned long long)r.lagGt1s, r.wallMs, r.cpuMs, r.decodeMs, r.feedMs, r.aggMs,
                (unsigned long long)r.invalids, (unsigned long long)r.seqFrames);
}
void printUsage() {
    struct rusage u{};
    getrusage(RUSAGE_SELF, &u);
    std::printf("process: user=%.0fms sys=%.0fms maxrss=%.1fMB\n",
                u.ru_utime.tv_sec * 1e3 + u.ru_utime.tv_usec / 1e3,
                u.ru_stime.tv_sec * 1e3 + u.ru_stime.tv_usec / 1e3, u.ru_maxrss / 1048576.0);
}
size_t compare(const std::vector<OHLCVBar>& a, const std::vector<OHLCVBar>& b, int64_t from,
               int64_t to, size_t& common) {
    std::map<int64_t, OHLCVBar> ma;
    for (const auto& x : a) if (x.timestamp_ms >= from && x.timestamp_ms < to) ma[x.timestamp_ms] = x;
    size_t diff = 0;
    common = 0;
    for (const auto& y : b) {
        if (y.timestamp_ms < from || y.timestamp_ms >= to) continue;
        const auto it = ma.find(y.timestamp_ms);
        if (it == ma.end()) continue;
        ++common;
        const auto& x = it->second;
        if (x.open != y.open || x.high != y.high || x.low != y.low || x.close != y.close ||
            x.volume != y.volume || x.count != y.count) ++diff;
    }
    return diff;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4) return 2;
    const std::string root = argv[1];
    const int64_t endMs = std::stoll(argv[2]);
    const std::string mode = argv[3];
    const int64_t windowStart = endMs - 30 * 60'000;
    const int64_t deliverFrom = argc > 4 ? std::stoll(argv[4]) : windowStart - 60'000;
    std::printf("mode=%s root=%s windowStart=%lld end=%lld deliverFrom=%lld\n", mode.c_str(),
                root.c_str(), (long long)windowStart, (long long)endMs, (long long)deliverFrom);
    const auto t0 = Clock::now();
    if (mode == "seq" || mode == "prefilter") {
        for (const auto& p : kProducts) print(run(root, p, deliverFrom, endMs, mode == "prefilter"));
    } else if (mode == "par") {
        std::vector<Result> results(kProducts.size());
        std::vector<std::thread> threads;
        for (size_t i = 0; i < kProducts.size(); ++i)
            threads.emplace_back([&, i] { results[i] = run(root, kProducts[i], deliverFrom, endMs, false); });
        for (auto& t : threads) t.join();
        for (const auto& r : results) print(r);
    } else if (mode == "cmp") {
        // Reference: deliver everything from the first record (continuous "live").
        for (const auto& p : kProducts) {
            const auto ref = run(root, p, INT64_MIN, endMs, false);
            const auto rec = run(root, p, deliverFrom, endMs, false);
            const auto noLead = run(root, p, windowStart, endMs, false);
            size_t c1 = 0, c2 = 0, c3 = 0, c4 = 0;
            const int64_t to = endMs - 2000;
            const size_t d100 = compare(ref.bars100, rec.bars100, windowStart, to, c1);
            const size_t d1s = compare(ref.bars1000, rec.bars1000, windowStart, to, c2);
            const size_t n100 = compare(ref.bars100, noLead.bars100, windowStart, to, c3);
            const size_t n1s = compare(ref.bars1000, noLead.bars1000, windowStart, to, c4);
            std::printf("%-13s lead60s: diff100=%zu/%zu diff1s=%zu/%zu | noLead: diff100=%zu/%zu "
                        "diff1s=%zu/%zu | ref trades=%llu late100=%llu late1s=%llu\n",
                        p.c_str(), d100, c1, d1s, c2, n100, c3, n1s, c4,
                        (unsigned long long)ref.trades, (unsigned long long)ref.late100,
                        (unsigned long long)ref.late1000);
        }
    }
    std::printf("total wall=%.0fms\n", ms(Clock::now() - t0));
    printUsage();
    return 0;
}
