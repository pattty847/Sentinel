// Incident evidence tooling: read-only on journals, with all output in scratch.
// This executable is never part of ctest.
#include "roller/Grid.hpp"
#include "roller/JournalFeed.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDateTime>
#include <fstream>
#include <iostream>
#include <numeric>

using namespace sentinel::roller;
using namespace recording;
using nlohmann::json;
namespace fs = std::filesystem;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 5) {
        std::cerr << "usage: one_sided_replay RAW_ROOT PRODUCT YYYY-MM-DD REPORT.json\n";
        return 2;
    }
    try {
        const fs::path raw = fs::canonical(argv[1]);
        const std::string product = argv[2], date = argv[3];
        const bool october20 = date == "2026-10-20";
        if ((!october20 && date != "2026-10-08") || (product != "ETH-USD" && product != "SOL-USD"))
            throw std::runtime_error("unsupported replay episode");
        const int hour = october20 ? 16 : 15;
        const auto start = QDateTime::fromString(QString::fromStdString(date + (october20 ? "T16:00:00Z" : "T15:00:00Z")), Qt::ISODate).toMSecsSinceEpoch();
        const int64_t end = start + 3'600'000;
        const int64_t windowStart = start + (october20 ? 18 * 60000 + 20000 : 13 * 60000 + 40000);
        const int64_t windowEnd = start + (october20 ? 34 * 60000 : 14 * 60000);
        QTemporaryDir scratch;
        if (!scratch.isValid()) throw std::runtime_error("scratch directory unavailable");
        const fs::path work = scratch.path().toStdString();
        const auto relative = fs::path(product) / "2026" / "10" / (october20 ? "20" : "08");
        fs::create_directories(work / "raw" / relative);
        // Include the preceding connection-start snapshot. Symlinks only: no journal copies.
        const int firstHour = october20 ? (product == "ETH-USD" ? 13 : 15) : 8;
        for (int h = firstHour; h <= hour + 1; ++h) {
            const auto file = (h < 10 ? "0" : "") + std::to_string(h) + ".rawl2";
            fs::create_symlink(raw / relative / file, work / "raw" / relative / file);
        }
        JournalReader inventory(work / "raw", product);
        const auto anchor = inventory.anchor(start);
        if (!anchor) throw std::runtime_error("no warmup snapshot");
        JournalReader reader(work / "raw", product, anchor);
        JournalFeed feed(product);
        JournalRecord input;
        std::unique_ptr<BookRecorder> recorder;
        json report{{"product", product}, {"date", date}, {"columns", json::array()},
                    {"waitTransitions", json::array()}, {"invalidations", json::array()}};
        std::string previousWait;
        uint64_t previousInvalidations = 0, records = 0;
        int64_t lastCheck = 0, localNow = 0;
        feed.onSnapshot = [&](int64_t envelope, int64_t local, std::vector<Level> levels) {
            if (!recorder) {
                double bid = 0, ask = std::numeric_limits<double>::infinity();
                for (const auto &l : levels) if (l.size > 0) {
                    if (l.isBid) bid = std::max(bid, l.price); else ask = std::min(ask, l.price);
                }
                auto cfg = deriveGrid(input.metadata, std::midpoint(bid, ask)).config(work / "out");
                cfg.commitFloorMs = start;
                cfg.commitCeilingMs = end;
                cfg.publisher = [&](auto r) {
                    if (r->header.tfMs != 60000) return;
                    json entries = json::array();
                    for (const auto &e : r->entries) entries.push_back({e.row, e.isAsk, e.twapCode, e.peakCode});
                    report["columns"].push_back({{"layer", r->header.layer}, {"bucket", r->bucketStartMs},
                        {"observed", r->observedMs}, {"flags", r->flags},
                        {"mid", {r->midOpen, r->midMin, r->midMax, r->midClose}},
                        {"bounds", {r->bidRowLo, r->bidRowHi, r->askRowLo, r->askRowHi}}, {"entries", std::move(entries)}});
                };
                recorder = std::make_unique<BookRecorder>(std::move(cfg));
            }
            recorder->onSnapshotAt(product, envelope, local, std::move(levels));
        };
        feed.onUpdates = [&](int64_t envelope, int64_t local, std::vector<Level> levels) {
            if (recorder) recorder->onUpdatesAt(product, envelope, local, std::move(levels));
        };
        feed.onInvalid = [&](int64_t local, const std::string &reason) {
            if (recorder) recorder->onInvalid(product, local, reason);
        };
        feed.onTick = [&](int64_t local) { if (recorder) recorder->onTick(local); };
        while (reader.next(input)) {
            localNow = input.record.time.systemNs / 1'000'000;
            if (localNow > end + 2500) break;
            feed.apply(input);
            ++records;
            // Fence each record around the incident, once per second elsewhere.
            if (!recorder || ((localNow < windowStart || localNow > windowEnd) && localNow - lastCheck < 1000)) continue;
            recorder->drain();
            lastCheck = localNow;
            const auto wait = recorder->snapshotWait(product).reason;
            const auto invalidations = recorder->stats().invalidations;
            if (localNow >= start && wait != previousWait)
                report["waitTransitions"].push_back({localNow, wait});
            if (localNow >= start && invalidations != previousInvalidations)
                report["invalidations"].push_back({localNow, invalidations - previousInvalidations});
            previousWait = wait;
            previousInvalidations = invalidations;
        }
        if (!recorder) throw std::runtime_error("no recorder");
        recorder->drain();
        report["records"] = records;
        report["diskErrors"] = recorder->stats().diskErrors;
        report["damaged"] = reader.damaged();
        report["columnCount"] = report["columns"].size();
        std::ofstream out(argv[4]);
        out << report.dump() << '\n';
        if (!out) throw std::runtime_error("report write failed");
        report.erase("columns");
        std::cout << report.dump(2) << '\n';
        return report["diskErrors"] == 0 && report["damaged"] == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
