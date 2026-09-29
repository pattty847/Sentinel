#include "RecordingLoader.hpp"
#include <stdexcept>

namespace heatmap {
SparseColumn fromRecording(const recording::Hmc2Record& record) {
    const auto& h = record.header;
    if ((h.tfMs != kMinuteMs && h.tfMs != kHourMs) || (h.tfMs == kHourMs && h.layer != "deep"))
        throw std::invalid_argument("invalid sparse recording level");
    SparseColumn out{record.bucketStartMs, record.observedMs, record.flags, {}};
    NativeColumn n;
    n.grid = {h.configHash, h.rowTickUnits, h.priceScale};
    n.sizeScale = h.sizeScale;
    n.observedMs = record.observedMs;
    if (h.tfMs == kHourMs && !(record.flags & recording::kApproximateCoverage)) {
        for (const auto& run : record.coverage)
            n.coverage[run.isAsk].push_back({run.lo, run.hi, run.coveredMs});
    } else {
        if (record.bidRowLo <= record.bidRowHi)
            n.coverage[0].push_back({record.bidRowLo, record.bidRowHi, record.observedMs});
        if (record.askRowLo <= record.askRowHi)
            n.coverage[1].push_back({record.askRowLo, record.askRowHi, record.observedMs});
    }
    int64_t base = std::numeric_limits<int64_t>::max();
    for (const auto& runs : n.coverage) for (const auto& run : runs) base = std::min(base, run.lo);
    for (const auto& e : record.entries) base = std::min(base, e.row);
    n.baseRow = base == std::numeric_limits<int64_t>::max() ? 0 : base;
    n.entries.reserve(record.entries.size());
    for (const auto& e : record.entries) {
        if (h.tfMs == kMinuteMs && (e.row < (e.isAsk ? record.askRowLo : record.bidRowLo) ||
                                  e.row > (e.isAsk ? record.askRowHi : record.bidRowHi))) continue;
        n.entries.push_back({packRowSide(e.row, n.baseRow, e.isAsk), uint16_t(e.twapCode & recording::kMaxCode),
                             h.tfMs == kHourMs ? e.coveredMs : record.observedMs});
    }
    out.native.push_back(std::move(n));
    return out;
}
SparseColumns loadRecording(recording::Hmc2Reader& reader, const std::string& symbol,
                             const std::string& layer, int64_t levelMs, int64_t startMs, int64_t endMs) {
    SparseColumns out{symbol, layer, levelMs, startMs, endMs, {}};
    validate(out);
    if ((levelMs != kMinuteMs && levelMs != kHourMs) || (levelMs == kHourMs && layer != "deep") ||
        startMs < recording::kHmc2MinMs || endMs > recording::kHmc2EndMs)
        throw std::invalid_argument("invalid sparse recording range");
    recording::ReadControl control;
    const auto scan = reader.visit(symbol, layer, levelMs, startMs, endMs, [&](const auto& r) {
        if (r.observedMs) out.columns.push_back(fromRecording(r));
    }, control);
    if (scan.status != recording::ReadStatus::Complete)
        throw std::runtime_error("sparse recording scan did not complete");
    validate(out);
    return out;
}
std::vector<SparseColumns> loadRecordingLevels(recording::Hmc2Reader& reader, const std::string& symbol,
    const std::string& layer, int64_t startMs, int64_t endMs, int64_t tfMs) {
    if (tfMs < kMinuteMs || tfMs > kDayMs || tfMs % kMinuteMs ||
        startMs % tfMs || endMs % tfMs || endMs <= startMs || (tfMs >= kHourMs && layer != "deep"))
        throw std::invalid_argument("invalid selected heatmap timeframe/range/layer");
    if (tfMs % kHourMs) return {loadRecording(reader, symbol, layer, kMinuteMs, startMs, endMs)};
    recording::ReadControl control;
    const auto minutes = reader.availability(symbol, layer, kMinuteMs, control);
    const auto hours = reader.availability(symbol, layer, kHourMs, control);
    if (control.status != recording::ReadStatus::Complete)
        throw std::runtime_error("sparse recording availability did not complete");
    auto tail = endMs;
    if (minutes.latestMs) {
        const auto candidate = recording::floorDiv(*minutes.latestMs, kHourMs) * kHourMs;
        if (!hours.latestMs || candidate > *hours.latestMs) tail = std::clamp(candidate, startMs, endMs);
    }
    std::vector<SparseColumns> levels;
    if (tail > startMs) levels.push_back(loadRecording(reader, symbol, layer, kHourMs, startMs, tail));
    if (tail < endMs) levels.push_back(loadRecording(reader, symbol, layer, kMinuteMs, tail, endMs));
    return levels;
}
} // namespace heatmap
