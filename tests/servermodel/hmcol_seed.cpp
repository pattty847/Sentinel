// hmcol_seed — write the deterministic heatmap history fixture to a data dir.
//
// Usage: hmcol_seed --dir <path> --mid <price> [--end-gap-min N] [--hours N]
//                   [--grid-height N] [--tick N] [--symbol S]
//
// Refuses a non-empty --dir so it can never mix into a live archive. The
// newest fixture bucket ends --end-gap-min minutes before now, leaving a
// missing gap between the fixture and live collection.

#include "HeatmapHistoryFixture.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    heatmap_fixture::Spec spec;
    std::string dir;
    int endGapMin = 20;
    int hours = 48;

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string val = argv[i + 1];
        if (key == "--dir") dir = val;
        else if (key == "--mid") spec.midPrice = std::stod(val);
        else if (key == "--end-gap-min") endGapMin = std::stoi(val);
        else if (key == "--hours") hours = std::stoi(val);
        else if (key == "--grid-height") spec.gridHeight = std::stoi(val);
        else if (key == "--tick") spec.tickSize = std::stod(val);
        else if (key == "--symbol") spec.symbol = val;
        else {
            std::cerr << "unknown option " << key << "\n";
            return 2;
        }
    }
    if (dir.empty() || spec.midPrice <= 0.0 || hours <= 0 || endGapMin < 0) {
        std::cerr << "usage: hmcol_seed --dir <path> --mid <price> [--end-gap-min 20] [--hours 48]"
                     " [--grid-height 2048] [--tick 5] [--symbol BTC-USD]\n";
        return 2;
    }
    if (fs::exists(dir) && !fs::is_empty(dir)) {
        std::cerr << "refusing to seed non-empty dir " << dir << "\n";
        return 1;
    }
    fs::create_directories(dir);

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    spec.endMs = (nowMs / spec.timeframeMs) * spec.timeframeMs - endGapMin * 60'000LL;
    spec.totalBuckets = hours * 60;

    HeatmapColumnStoreConfig storeCfg;
    storeCfg.fsyncEveryNRecords = 1 << 30;  // one flush at the end
    storeCfg.fsyncEveryMs = 1 << 30;
    HeatmapColumnStore store(dir, storeCfg);
    if (!store.acquireLock()) {
        std::cerr << "could not lock " << dir << "\n";
        return 1;
    }

    const auto res = heatmap_fixture::seed(store, spec);
    const heatmap_fixture::Layout layout;
    std::cout << "seeded " << dir << " symbol=" << spec.symbol
              << " tf=" << spec.timeframeMs << " grid_height=" << spec.gridHeight
              << " tick=" << spec.tickSize << "\n"
              << "  range_ms=[" << spec.endMs - spec.totalBuckets * spec.timeframeMs
              << ", " << spec.endMs << ")\n"
              << "  written=" << res.written << " recorded=" << res.recorded
              << " zero=" << res.zero << " missing=" << res.missing
              << " failed=" << res.failed << "\n"
              << "  reference_price=" << heatmap_fixture::referencePrice(spec)
              << " newer_band_mid=" << spec.midPrice
              << " older_band_mid=" << spec.midPrice + spec.bandShiftFrac * heatmap_fixture::bandSpan(spec)
              << " (buckets >= " << layout.bandShiftAt << " min before end)\n";
    return res.failed == 0 ? 0 : 1;
}
