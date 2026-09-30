#include "LabSources.hpp"
#include "ConfigLoader.hpp"
#include "heatmap/RecordingLoader.hpp"
#include "servermodel/RecordingDir.hpp"
#include "heatmap/TimeComposer.hpp"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
LabSource finish(heatmap::SparseColumns composed, heatmap::gpu::GpuSourceOptions options, LabSource out) {
    uint64_t entries = 0;
    for (const auto &c : composed.columns) for (const auto &n : c.native) entries += n.entries.size();
    out.sparseEntries = entries;
    out.medianPrice = medianRecentPrice(composed);
    const auto built = Clock::now();
    out.gpu = std::make_shared<const heatmap::gpu::GpuSource>(heatmap::gpu::buildGpuSource(composed, options));
    out.buildMs = msSince(built);
    out.columns = std::make_shared<const heatmap::SparseColumns>(std::move(composed));
    return out;
}
} // namespace

// Median over recent columns of the best-bid/best-ask midpoint (highest bid
// entry, lowest ask entry): the market price the initial view centres on.
double medianRecentPrice(const heatmap::SparseColumns &data) {
    std::vector<double> mids;
    const size_t first = data.columns.size() > 30 ? data.columns.size() - 30 : 0;
    for (size_t c = first; c < data.columns.size(); ++c)
        for (const auto &n : data.columns[c].native) {
            int64_t bestBid = INT64_MIN, bestAsk = INT64_MAX;
            for (const auto &e : n.entries) {
                const int64_t row = n.baseRow + e.row();
                if (e.isAsk()) bestAsk = std::min(bestAsk, row);
                else bestBid = std::max(bestBid, row);
            }
            if (bestBid != INT64_MIN && bestAsk != INT64_MAX)
                mids.push_back(double(bestBid + bestAsk + 1) / 2 * n.grid.rowTickUnits / n.grid.priceScale);
        }
    if (mids.empty()) return 0;
    std::nth_element(mids.begin(), mids.begin() + mids.size() / 2, mids.end());
    return mids[mids.size() / 2];
}

std::string recordingRoot() {
    const QString override = qEnvironmentVariable("SENTINEL_RECORDING_ROOT").trimmed();
    if (!override.isEmpty()) return override.toStdString();
    // Otherwise where the server records: the same config files, read relative to
    // the working directory, and the same dir/fallback rule (RecordingDir.hpp).
    static const std::string configured = [] {
        ServerConfig config;
        if (!ConfigLoader::loadServerConfig("config/server_config.yaml", &config)) return std::string();
        ConfigLoader::loadServerConfig("config/.server_config.yaml", &config);
        const auto choice = recording::resolveRecordingDir(config.recording.dir, config.recording.fallbackDir);
        if (choice.dir.empty()) return std::string();
        return std::filesystem::absolute(choice.dir).lexically_normal().string();
    }();
    return configured;
}

bool insideRecordingRoot(const QString &path) {
    const QString root = QString::fromStdString(recordingRoot());
    if (root.isEmpty()) return false;
#ifdef Q_OS_WIN
    constexpr auto cs = Qt::CaseInsensitive;
#else
    constexpr auto cs = Qt::CaseSensitive;
#endif
    const QString base = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    auto inside = [&](const QString &p) {
        const QString clean = QDir::cleanPath(p);
        return !clean.isEmpty() && (clean.compare(base, cs) == 0 || clean.startsWith(base + '/', cs));
    };
    const QFileInfo file(path);
    return inside(file.absoluteFilePath()) || inside(file.absoluteDir().canonicalPath());
}

LabSource loadRealSource(const std::string &layer, int hours, int loadHours, int64_t tfMs, const std::string &rootIn) {
    const std::string root = rootIn.empty() ? recordingRoot() : rootIn;
    if (root.empty()) throw std::runtime_error("no recording root: set SENTINEL_RECORDING_ROOT or recording.dir in "
                                             "config/server_config.yaml (the directory holding BTC-USD/)");
    if (!std::filesystem::is_directory(std::filesystem::u8path(root) / "BTC-USD"))
        throw std::runtime_error("no BTC-USD recording under the recording root " + root);
    if (hours < 1 || loadHours < 1 || tfMs < heatmap::kMinuteMs || tfMs > heatmap::kDayMs ||
        tfMs % heatmap::kMinuteMs || (layer != "near" && layer != "deep"))
        throw std::invalid_argument("invalid lab recording request");
    recording::Hmc2Reader reader(root); // read-only; never takes the writer lock
    recording::ReadControl control;
    const auto minutes = reader.availability("BTC-USD", layer, heatmap::kMinuteMs, control);
    const auto hoursAvail = layer == "deep" ? reader.availability("BTC-USD", layer, heatmap::kHourMs, control)
                                            : recording::SeriesAvailability{};
    if (control.status != recording::ReadStatus::Complete || !minutes.latestMs)
        throw std::runtime_error("no " + layer + " minute recording available");
    LabSource out;
    const int64_t newest = *minutes.latestMs + heatmap::kMinuteMs;
    out.endMs = recording::floorDiv(newest + tfMs - 1, tfMs) * tfMs;
    const int64_t fullStart = recording::floorDiv(out.endMs - int64_t(hours) * heatmap::kHourMs, tfMs) * tfMs;
    out.startMs = std::max(fullStart,
        recording::floorDiv(out.endMs - int64_t(std::min(hours, loadHours)) * heatmap::kHourMs, tfMs) * tfMs);
    int64_t oldest = minutes.oldestMs.value_or(out.startMs);
    if (hoursAvail.oldestMs) oldest = std::min(oldest, *hoursAvail.oldestMs);
    out.availableStartMs = std::max(oldest, fullStart);
    const auto loaded = Clock::now();
    std::vector<heatmap::SparseColumns> levels;
    if (layer == "deep" && tfMs % heatmap::kHourMs == 0)
        levels = heatmap::loadRecordingLevels(reader, "BTC-USD", layer, out.startMs, out.endMs, tfMs);
    else // near has no hour rollups; odd and sub-hour timeframes compose minutes
        levels.push_back(heatmap::loadRecording(reader, "BTC-USD", layer, heatmap::kMinuteMs, out.startMs, out.endMs));
    out.loadMs = msSince(loaded);
    const auto composedAt = Clock::now();
    auto composed = heatmap::compose(levels, tfMs);
    out.composeMs = msSince(composedAt);
    heatmap::gpu::GpuSourceOptions options;
    options.availableStartMs = out.availableStartMs;
    options.availableEndMs = out.endMs;
    return finish(std::move(composed), options, std::move(out));
}

LabSource syntheticSource(uint64_t entries, int64_t tfMs) {
    constexpr int64_t minute = heatmap::kMinuteMs;
    const int64_t epoch = recording::kHmc2MinMs + 10 * heatmap::kDayMs;
    const uint64_t perSide = std::max<uint64_t>(1, entries / 1440 / 2);
    heatmap::SparseColumns data{"SYNTH", "deep", minute, epoch, epoch + heatmap::kDayMs, {}, {}};
    data.scannedRanges = {{epoch, epoch + 600 * minute}, {epoch + 660 * minute, epoch + heatmap::kDayMs}};
    const auto loaded = Clock::now();
    for (int64_t i = 0; i < 1440; ++i) {
        if ((i >= 600 && i < 660) || i % 97 == 13) continue; // unloaded hour, recorder gaps
        heatmap::NativeColumn n;
        const int64_t tickUnits = i < 720 ? 1000 : 500;     // $10 then $5 native grid
        n.grid = {uint64_t(i < 720 ? 1 : 2), tickUnits, 100};
        if (i >= 1080) n.sizeScale.floor = 1e-8;               // size-scale change, same grid
        n.observedMs = i % 11 ? minute : 21'000;
        const double tick = tickUnits / 100.0;
        const double mid = 100'000 + 900 * std::sin(double(i) / 170.0) + 120 * std::sin(double(i) / 9.0);
        const int64_t midRow = int64_t(mid / tick);
        const int64_t lo = midRow - int64_t(perSide), hi = midRow + int64_t(perSide) - 1;
        n.baseRow = lo;
        // Both sides observe the whole window (as the recorder does); a few
        // minutes see a narrower window, which leaves unproven (veiled) edges.
        n.coverage[0] = {{lo + (i % 19 == 0 ? 40 : 0), hi, n.observedMs}};
        n.coverage[1] = {{lo, hi - (i % 23 == 0 ? 40 : 0), n.observedMs}};
        n.entries.reserve(size_t(2 * perSide));
        for (int64_t row = lo; row <= hi; ++row) {
            const bool ask = row >= midRow;
            const double distance = std::abs(double(row - midRow)) + 1;
            double size = 0.02 + 3.0 / std::sqrt(distance);
            if ((row * 7919 + i / 30 * 104729) % 211 == 0) size *= 40; // walls that persist ~30 min
            n.entries.push_back({heatmap::packRowSide(row, lo, ask), recording::encodeSize(size, n.sizeScale)});
        }
        data.columns.push_back({epoch + i * minute, n.observedMs, 0, {std::move(n)}});
    }
    LabSource out;
    out.startMs = epoch;
    out.endMs = epoch + heatmap::kDayMs;
    out.availableStartMs = epoch;
    out.loadMs = msSince(loaded);
    const auto composedAt = Clock::now();
    auto composed = heatmap::compose(data, tfMs);
    out.composeMs = msSince(composedAt);
    return finish(std::move(composed), {}, std::move(out));
}
} // namespace lab
