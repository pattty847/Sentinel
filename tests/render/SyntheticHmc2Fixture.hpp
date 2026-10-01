#pragma once
// The synthetic HMC2 recording of LabItemTest, test_ugr_gpu and
// test_heatmap_settings_ui: BTC-USD near $1 rows (a band) and deep $10 rows (a
// wide book) per minute from `epoch`. Tests write it into a QTemporaryDir and
// configure LabData once per binary (FM-125).
#include "heatmap/HeatmapResolution.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <QTemporaryDir>

namespace synthetic_hmc2 {
using heatmap::kHourMs;
using heatmap::kMinuteMs;
constexpr int64_t minute = kMinuteMs;
constexpr int64_t epoch = recording::kHmc2MinMs + 50 * heatmap::kDayMs;

// Minute i of layer "deep" ($10 rows, a wide book) or "near" ($1 rows, a band):
// the fixture of LabItemTest.
inline recording::Hmc2Record syntheticMinute(const std::string &layer, int64_t i) {
    const bool deep = layer == "deep";
    recording::Hmc2Record r;
    r.header = {"BTC-USD", layer, minute, 100, deep ? 1000 : 100, {}, deep ? 77u : 78u};
    r.bucketStartMs = epoch + i * minute;
    r.observedMs = uint32_t(minute);
    const int64_t mid = (100'000 + (i % 30) * 10) * 100 / r.header.rowTickUnits;
    const int64_t half = deep ? 40 : 200;
    r.bidRowLo = r.askRowLo = mid - half;
    r.bidRowHi = r.askRowHi = mid + half - 1;
    r.midOpen = r.midClose = r.midMin = r.midMax = double(mid) * double(r.header.rowTickUnits) / 100;
    for (int64_t row = mid - half; row < mid + half; ++row) {
        const bool ask = row >= mid;
        const auto code = recording::encodeSize(0.01 * double(1 + (row * 7 + i) % 53), r.header.sizeScale);
        r.entries.push_back({row, ask, code, code, r.observedMs});
    }
    return r;
}
inline void writeRecording(const QTemporaryDir &dir, int64_t minutes) {
    recording::Hmc2Store writer(dir.path().toStdString());
    for (int64_t i = 0; i < minutes; ++i)
        for (const char *layer : {"deep", "near"}) writer.append(syntheticMinute(layer, i));
}

} // namespace synthetic_hmc2
