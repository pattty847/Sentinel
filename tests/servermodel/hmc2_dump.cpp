// hmc2_dump: print recording v2 (HMC2) columns so a human or agent can check them.
// Usage: hmc2_dump <root> <symbol> <layer> [tfMs=60000] [lastN=3] [topK=8]
// Example: hmc2_dump /Volumes/T7/sentinel-data/recording BTC-USD deep
#include "servermodel/Hmc2Store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace recording;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <root> <symbol> <layer> [tfMs=60000] [lastN=3] [topK=8]\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1], symbol = argv[2], layer = argv[3];
    const int64_t tf = argc > 4 ? std::atoll(argv[4]) : 60'000;
    const int lastN = argc > 5 ? std::atoi(argv[5]) : 3;
    const int topK = argc > 6 ? std::atoi(argv[6]) : 8;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const auto records = Hmc2Store::readRange(root, symbol, layer, tf, now - 7LL * 86'400'000, now + tf);
    std::printf("%s %s tf=%lld: %zu records in the last 7 days\n", symbol.c_str(), layer.c_str(),
                static_cast<long long>(tf), records.size());
    const size_t first = records.size() > static_cast<size_t>(lastN) ? records.size() - lastN : 0;
    for (size_t i = first; i < records.size(); ++i) {
        const auto& r = records[i];
        const double tick = static_cast<double>(r.header.rowTickUnits) / r.header.priceScale;
        double bidQty = 0, askQty = 0;
        for (const auto& e : r.entries) (e.isAsk ? askQty : bidQty) += decodeSize(e.twapCode, r.header.sizeScale);
        std::printf("\nbucket=%lld observedMs=%u flags=0x%x entries=%zu mid[o=%.2f c=%.2f lo=%.2f hi=%.2f]\n",
                    static_cast<long long>(r.bucketStartMs), r.observedMs, r.flags, r.entries.size(),
                    r.midOpen, r.midClose, r.midMin, r.midMax);
        std::printf("  bid rows %.2f..%.2f  ask rows %.2f..%.2f  (tick %.2f)  twap total bid=%.3f ask=%.3f\n",
                    r.bidRowLo * tick, (r.bidRowHi + 1) * tick, r.askRowLo * tick, (r.askRowHi + 1) * tick, tick,
                    bidQty, askQty);
        auto top = r.entries;
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.twapCode > b.twapCode; });
        for (int k = 0; k < topK && k < static_cast<int>(top.size()); ++k) {
            const auto& e = top[k];
            std::printf("  %s %10.2f  twap=%10.4f  peak=%10.4f\n", e.isAsk ? "ask" : "bid", e.row * tick,
                        decodeSize(e.twapCode, r.header.sizeScale), decodeSize(e.peakCode, r.header.sizeScale));
        }
    }
    return 0;
}
