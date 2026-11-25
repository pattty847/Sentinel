// Read-only grid audit: compare the roller's snapshot rule with every HMC2 day.
// No writer or store worker is constructed. Run one product per build-queue job.
#include "roller/JournalReader.hpp"
#include "roller/JournalFeed.hpp"
#include "roller/Grid.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "servermodel/FlowStore.hpp"
#include "flow/FlowDayFile.hpp"
#include "flow/FlowScales.hpp"
#include <fstream>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <iostream>
#include <numeric>
#include <set>
namespace r = sentinel::roller;
constexpr int64_t Day = 86'400'000;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc != 4 && argc != 5) throw std::runtime_error("usage: flow_grid_check RAW HMC SYMBOL [FLC]");
        const std::string symbol = argv[3];
        r::JournalReader catalog(argv[1], symbol);
        recording::Hmc2Reader hmc(argv[2]);
        std::set<int64_t> days;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(std::filesystem::path(argv[2]) / symbol)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".hmc2") continue;
            const auto date = QDate::fromString(QString::fromStdString(entry.path().stem().string()), "yyyy-MM-dd");
            if (date.isValid()) days.insert(date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch());
        }
        std::map<int64_t, sentinel::flow::chunks::DayFile> fallbackSealed;
        if (argc == 5) for (const auto &entry : std::filesystem::directory_iterator(
                std::filesystem::path(argv[4]) / "coinbase" / symbol)) {
            if (entry.path().extension() != ".flc") continue;
            const auto date = QDate::fromString(QString::fromStdString(entry.path().stem().string()), "yyyy-MM-dd");
            if (!date.isValid()) continue;
            const auto day = date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch();
            if (days.contains(day)) continue;
            std::ifstream file(entry.path(), std::ios::binary);
            const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
            fallbackSealed.emplace(day, sentinel::flow::chunks::parseDayFile(bytes));
        }
        for (const auto &[day, _] : fallbackSealed) days.insert(day);
        size_t compared = 0, mismatches = 0, failed = 0, sealedCompared = 0, sealedMismatches = 0;
        for (auto day : days) {
            std::optional<recording::Hmc2Header> header;
            struct Found {};
            recording::ReadControl control;
            try { hmc.visit(symbol, "near", 60'000, day, day + Day,
                [&](const auto &record) { header = record.header; throw Found{}; }, control); }
            catch (const Found &) {}
            if (!header && !fallbackSealed.contains(day)) { ++failed; std::cout << "unreadable day=" << day << std::endl; continue; }
            recording::FlowStoreGridValues grid;
            try { grid = recording::FlowStore::journalGridFor(symbol, day, catalog); }
            catch (const std::exception &e) { ++failed; std::cout << "no_grid day=" << day << " error=" << e.what() << std::endl; continue; }
            if (fallbackSealed.contains(day)) {
                ++sealedCompared;
                const auto &scales = grid.scales;
                const auto &old = fallbackSealed.at(day).grid;
                const bool different = old.rowTickUnits != scales.rowTickUnits || old.priceScale != scales.priceScale ||
                    old.baseScale != scales.baseScale || old.quoteScale != scales.quoteScale;
                sealedMismatches += different;
                std::cout << "sealed_fallback day=" << day << " mismatch=" << different
                          << " old_tick=" << old.rowTickUnits << " new_tick=" << scales.rowTickUnits << std::endl;
                continue;
            }
            ++compared;
            if (grid.scales.priceScale != header->priceScale ||
                std::llround(grid.scales.nativeTick * grid.scales.priceScale) != header->rowTickUnits ||
                grid.sizeFloor != header->sizeScale.floor) {
                ++mismatches;
                std::cout << "mismatch day=" << day
                    << " derived=" << grid.scales.priceScale << ',' << grid.scales.nativeTick << ',' << grid.sizeFloor
                    << " hmc=" << header->priceScale << ',' << double(header->rowTickUnits)/header->priceScale
                    << ',' << header->sizeScale.floor << std::endl;
            }
        }
        std::cout << "symbol=" << symbol << " days=" << days.size() << " compared=" << compared
                  << " mismatches=" << mismatches << " failed=" << failed
                  << " sealed_fallback_compared=" << sealedCompared << " sealed_fallback_mismatches=" << sealedMismatches << std::endl;
        return mismatches || failed || sealedMismatches || !compared ? 1 : 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 2; }
}
