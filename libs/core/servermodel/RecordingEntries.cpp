#include "RecordingEntries.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace recording {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point then) {
    return std::chrono::duration<double, std::milli>(Clock::now() - then).count();
}
constexpr int64_t minute = 60'000;
struct Entry { uint32_t col; int64_t row; uint32_t side; float size; };
void finish(RecordingEntries &out, std::vector<Entry> &entries,
            std::vector<std::array<int64_t, 4>> &bounds,
            const std::vector<int64_t> &nativeUnits, int64_t commonUnits) {
    for (auto &e : entries) e.row = floorDiv(e.row, commonUnits);
    for (size_t c = 0; c < bounds.size(); ++c) {
        if (!nativeUnits[c]) continue;
        auto &b = bounds[c];
        for (int i : {0, 2}) {
            if (b[i] > b[i + 1]) continue;
            // A common row is valid only if every underlying native row was
            // covered, including the zero-valued rows absent from entries.
            const int64_t loUnits = b[i] * nativeUnits[c];
            const int64_t endUnits = (b[i + 1] + 1) * nativeUnits[c];
            b[i] = -floorDiv(-loUnits, commonUnits);
            b[i + 1] = floorDiv(endUnits, commonUnits) - 1;
        }
    }
    int64_t base = std::numeric_limits<int64_t>::max();
    for (const auto &e : entries) base = std::min(base, e.row);
    for (const auto &b : bounds)
        for (int i : {0, 2}) if (b[i] <= b[i + 1]) base = std::min(base, b[i]);
    out.baseRow = base == std::numeric_limits<int64_t>::max() ? 0 : base;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.col != b.col) return a.col < b.col;
        if (a.row != b.row) return a.row < b.row;
        return a.side < b.side;
    });
    out.column.reserve(entries.size()); out.row.reserve(entries.size());
    out.side.reserve(entries.size()); out.size.reserve(entries.size());
    out.offsets.resize(out.columns() + 1);
    size_t next = 0;
    for (uint32_t c = 0; c < out.columns(); ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.column.size());
        while (next < entries.size() && entries[next].col == c) {
            const auto &e = entries[next++];
            const auto rel = e.row - out.baseRow;
            if (rel < 0 || rel > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("recording native row range exceeds GPU index width");
            if (!out.column.empty() && out.column.back() == e.col && out.row.back() == uint32_t(rel) &&
                out.side.back() == e.side) {
                out.size.back() += e.size;
            } else {
                out.column.push_back(e.col); out.row.push_back(static_cast<uint32_t>(rel));
                out.side.push_back(e.side); out.size.push_back(e.size);
            }
        }
        const auto &b = bounds[c];
        auto convert = [&](int i) -> std::pair<int32_t, int32_t> {
            if (b[i] > b[i + 1]) return {1, 0};
            const auto lo = b[i] - out.baseRow, hi = b[i + 1] - out.baseRow;
            if (lo < 0 || hi > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("recording coverage exceeds GPU index width");
            return {static_cast<int32_t>(lo), static_cast<int32_t>(hi)};
        };
        const auto bid = convert(0), ask = convert(2);
        out.coverage[c] = {bid.first, bid.second, ask.first, ask.second};
    }
    out.offsets.back() = static_cast<uint32_t>(out.column.size());
}
} // namespace

RecordingEntries loadRecordingEntries(const std::filesystem::path &root,
                                      const std::string &symbol, const std::string &layer,
                                      int64_t startMs, int64_t endMs) {
    if ((layer != "near" && layer != "deep") || symbol.empty() || startMs >= endMs ||
        startMs < kHmc2MinMs || endMs > kHmc2EndMs || startMs % minute || endMs % minute ||
        (endMs - startMs) / minute > 100'000)
        throw std::invalid_argument("invalid recording entry range");
    const auto started = Clock::now();
    RecordingEntries out;
    out.startMs = startMs;
    out.coverage.resize(static_cast<size_t>((endMs - startMs) / minute));
    out.observedMs.resize(out.columns());
    std::vector<std::array<int64_t, 4>> bounds(out.columns(), {1, 0, 1, 0});
    std::vector<int64_t> nativeUnits(out.columns(), 0);
    std::vector<Entry> entries;
    std::string failure;
    int64_t commonUnits = 0;
    Hmc2Reader reader(root);
    ReadControl control;
    const auto scan = reader.visit(symbol, layer, minute, startMs, endMs, [&](const Hmc2Record &r) {
        if (!failure.empty()) return;
        const auto decoded = Clock::now();
        const double tick = r.header.rowTickUnits / r.header.priceScale;
        if (!std::isfinite(tick) || tick <= 0 || !std::isfinite(r.header.priceScale) ||
            r.header.priceScale <= 0) { failure = "bad recording native grid"; return; }
        if (out.priceScale == 100 && commonUnits == 0) {
            out.priceScale = r.header.priceScale; out.sizeScale = r.header.sizeScale;
        } else if (r.header.priceScale != out.priceScale) {
            failure = "mixed price scales in recording range"; return;
        }
        const int64_t units = r.header.rowTickUnits;
        if (commonUnits) {
            const int64_t gcd = std::gcd(commonUnits, units);
            if (commonUnits / gcd > std::numeric_limits<int64_t>::max() / units) {
                failure = "common native grid overflow"; return;
            }
            commonUnits = commonUnits / gcd * units;
        } else commonUnits = units;
        const auto col = static_cast<uint32_t>((r.bucketStartMs - startMs) / minute);
        if (col >= out.columns() || !r.observedMs) return;
        bounds[col] = {r.bidRowLo, r.bidRowHi, r.askRowLo, r.askRowHi};
        nativeUnits[col] = units;
        out.observedMs[col] = r.observedMs;
        for (const auto &e : r.entries) {
            const auto lo = e.isAsk ? r.askRowLo : r.bidRowLo;
            const auto hi = e.isAsk ? r.askRowHi : r.bidRowHi;
            if (e.row < lo || e.row > hi) continue;
            const double decodedSize = decodeSize(e.twapCode, r.header.sizeScale);
            if (!std::isfinite(decodedSize) || decodedSize < 0 ||
                decodedSize > std::numeric_limits<float>::max()) {
                failure = "nonfinite recording size"; return;
            }
            if (decodedSize > 0) {
                if (e.row > std::numeric_limits<int64_t>::max() / units ||
                    e.row < std::numeric_limits<int64_t>::min() / units) {
                    failure = "native price unit overflow"; return;
                }
                entries.push_back({col, e.row * units, uint32_t(e.isAsk), float(decodedSize)});
            }
        }
        out.decodeMs += elapsed(decoded);
    }, control);
    if (!failure.empty()) throw std::runtime_error(failure);
    if (scan.status != ReadStatus::Complete)
        throw std::runtime_error("recording reader did not complete range scan");
    if (commonUnits) out.nativeTick = commonUnits / out.priceScale;
    finish(out, entries, bounds, nativeUnits, commonUnits ? commonUnits : 1);
    out.loadMs = elapsed(started);
    return out;
}

RecordingEntries syntheticRecordingEntries(uint32_t count) {
    const auto started = Clock::now();
    if (!count || count > 100'000'000) throw std::invalid_argument("synthetic entry count out of range");
    RecordingEntries out;
    out.startMs = kHmc2MinMs;
    out.nativeTick = 1; out.priceScale = 100;
    constexpr uint32_t cols = 1000;
    const uint32_t rows = (count + cols - 1) / cols;
    out.coverage.resize(cols, {0, int32_t(rows - 1), 0, int32_t(rows - 1)});
    out.observedMs.resize(cols, 60'000);
    out.offsets.resize(cols + 1);
    out.column.reserve(count); out.row.reserve(count); out.side.reserve(count); out.size.reserve(count);
    for (uint32_t c = 0; c < cols; ++c) {
        out.offsets[c] = static_cast<uint32_t>(out.column.size());
        for (uint32_t r = 0; r < rows && out.column.size() < count; ++r) {
            out.column.push_back(c); out.row.push_back(r);
            out.side.push_back(r > rows / 2 ? 1u : 0u);
            const bool wall = r == rows / 3 || r == rows * 2 / 3 || r == rows / 2 + 41;
            out.size.push_back(wall ? 116'000.0f : float(0.01 + ((r * 17u + c * 13u) % 100u) * 0.003));
        }
    }
    out.offsets.back() = count;
    out.loadMs = elapsed(started);
    return out;
}
} // namespace recording
