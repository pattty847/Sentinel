#pragma once
// Deterministic heatmap history fixture for scrollback validation.
//
// Writes a known 1m archive through HeatmapColumnStore so the on-disk format,
// CRCs, slot rules and the single-writer lock are the production ones. Every
// feature is placed so a paging or mapping error is visible on the chart:
//   - reference line: bright ask band at one absolute price for the whole
//     archive; it must stay straight across the price-band shift
//   - sine line: bright bid line with a 6 h period; a misplaced page breaks it
//   - hour markers: one full-height column at every UTC hour (stronger at
//     00:00 UTC, where the day file changes)
//   - background: faint asks above / bids below the reference line
//   - zero stretch: recorded all-zero columns (empty, not missing-shaded)
//   - missing gaps: unwritten buckets (muted missing-shaded on the client)
//   - band shift: the older half uses a band centred lower than the newer half

#include "servermodel/HeatmapColumnStore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace heatmap_fixture {

struct Spec {
    std::string symbol = "BTC-USD";
    int64_t timeframeMs = 60'000;
    int32_t gridHeight = 2048;
    double tickSize = 5.0;
    double midPrice = 100'000.0;  // centre of the newer band; use the live price
    int64_t endMs = 0;            // exclusive end of the newest bucket (tf-aligned)
    int totalBuckets = 2880;      // 48 h at 1m, about three 1024-column pages
    double bandShiftFrac = -0.2;  // older band centre offset, as a fraction of the band span
};

inline double bandSpan(const Spec& s) { return static_cast<double>(s.gridHeight) * s.tickSize; }

enum class Kind { Recorded, Zero, Missing };

struct Bucket {
    int64_t startMs = 0;
    Kind kind = Kind::Recorded;
    double minPrice = 0.0;
    double maxPrice = 0.0;
};

// Layout in bucket indices counted back from the newest bucket (index 0).
// Page boundaries sit near 1024 and 2048 buckets before the end.
struct Layout {
    int gapShortFrom = 300;   // 30 min gap inside the newest page
    int gapShortLen = 30;
    int gapLongFrom = 940;    // 3 h gap straddling the first page boundary
    int gapLongLen = 180;
    int zeroFrom = 1700;      // 1 h of recorded all-zero columns
    int zeroLen = 60;
    int bandShiftAt = 1440;   // buckets at or older than this use the older band
};

// Feature prices scale with the band span so they fit any tick size and
// stay inside both the newer and the shifted older band.
inline double referencePrice(const Spec& s) { return s.midPrice + 0.1 * bandSpan(s); }

inline double sinePrice(const Spec& s, int64_t bucketStartMs) {
    constexpr double kPi = 3.14159265358979323846;
    const double periodMs = 6.0 * 3'600'000.0;
    const double phase = std::fmod(static_cast<double>(bucketStartMs), periodMs) / periodMs;
    return s.midPrice - 0.2 * bandSpan(s) + 0.1 * bandSpan(s) * std::sin(2.0 * kPi * phase);
}

inline std::vector<Bucket> plan(const Spec& s, const Layout& l = Layout{}) {
    std::vector<Bucket> out;
    out.reserve(static_cast<size_t>(s.totalBuckets));
    const double span = bandSpan(s);
    for (int back = s.totalBuckets - 1; back >= 0; --back) {
        Bucket b;
        b.startMs = s.endMs - static_cast<int64_t>(back + 1) * s.timeframeMs;
        const bool inShort = back >= l.gapShortFrom && back < l.gapShortFrom + l.gapShortLen;
        const bool inLong = back >= l.gapLongFrom && back < l.gapLongFrom + l.gapLongLen;
        const bool inZero = back >= l.zeroFrom && back < l.zeroFrom + l.zeroLen;
        b.kind = (inShort || inLong) ? Kind::Missing : (inZero ? Kind::Zero : Kind::Recorded);
        const double centre = (back >= l.bandShiftAt) ? s.midPrice + s.bandShiftFrac * span : s.midPrice;
        b.minPrice = centre - span * 0.5;
        b.maxPrice = centre + span * 0.5;
        out.push_back(b);
    }
    return out;
}

// Row 0 is the top of the band (maxPrice), matching LiveOrderBook::accumulateRangeSplit.
inline int rowForPrice(const Bucket& b, double tickSize, int32_t gridHeight, double price) {
    const int row = static_cast<int>(std::floor((b.maxPrice - price) / tickSize));
    return (row >= 0 && row < gridHeight) ? row : -1;
}

inline uint16_t encodeAsk(double norm) {
    return static_cast<uint16_t>(0x8000u + static_cast<uint16_t>(std::lround(std::clamp(norm, 0.0, 1.0) * 32767.0)));
}
inline uint16_t encodeBid(double norm) {
    return static_cast<uint16_t>(std::lround(std::clamp(norm, 0.0, 1.0) * 32767.0));
}
inline double decodeNorm(uint16_t v) {
    return static_cast<double>(v & 0x7FFFu) / 32767.0;
}

// Fills intensity (signed u16) and liquidity (u16 scaled) cells for one bucket.
inline void buildCells(const Spec& s, const Bucket& b,
                       std::vector<uint16_t>& intensity,
                       std::vector<uint16_t>& liquidity,
                       double& liquidityScale) {
    const auto h = static_cast<size_t>(s.gridHeight);
    intensity.assign(h, 0);
    liquidity.assign(h, 0);
    liquidityScale = 1.0;
    if (b.kind != Kind::Recorded) {
        return;
    }

    std::vector<double> norm(h, 0.0);
    std::vector<bool> ask(h, false);
    const int refRow = rowForPrice(b, s.tickSize, s.gridHeight, referencePrice(s));
    for (size_t r = 0; r < h; ++r) {
        norm[r] = 0.10;
        ask[r] = refRow >= 0 && static_cast<int>(r) < refRow;
    }

    const int64_t minuteOfDay = (b.startMs / 60'000) % 1440;
    if (minuteOfDay % 60 == 0) {
        const double marker = (minuteOfDay == 0) ? 0.70 : 0.35;
        std::fill(norm.begin(), norm.end(), marker);
    }

    auto paint = [&](double price, int halfRows, double value, bool isAsk) {
        const int centre = rowForPrice(b, s.tickSize, s.gridHeight, price);
        if (centre < 0) return;
        for (int r = std::max(0, centre - halfRows);
             r <= std::min(s.gridHeight - 1, centre + halfRows); ++r) {
            norm[static_cast<size_t>(r)] = value;
            ask[static_cast<size_t>(r)] = isAsk;
        }
    };
    paint(sinePrice(s, b.startMs), 2, 0.85, false);
    paint(referencePrice(s), 1, 1.0, true);

    constexpr double kMaxQty = 100.0;
    liquidityScale = kMaxQty / 65535.0;
    for (size_t r = 0; r < h; ++r) {
        intensity[r] = ask[r] ? encodeAsk(norm[r]) : encodeBid(norm[r]);
        liquidity[r] = static_cast<uint16_t>(std::lround(norm[r] * 65535.0));
    }
}

struct SeedResult {
    int written = 0;
    int recorded = 0;
    int zero = 0;
    int missing = 0;
    int failed = 0;
};

// Caller owns the store and must hold its lock.
inline SeedResult seed(HeatmapColumnStore& store, const Spec& s, const Layout& l = Layout{}) {
    SeedResult res;
    std::vector<uint16_t> intensity;
    std::vector<uint16_t> liquidity;
    for (const auto& b : plan(s, l)) {
        if (b.kind == Kind::Missing) {
            ++res.missing;
            continue;
        }
        double scale = 1.0;
        buildCells(s, b, intensity, liquidity, scale);
        const auto r = store.append(s.symbol, s.timeframeMs, s.gridHeight,
                                    b.startMs, b.startMs + s.timeframeMs,
                                    b.minPrice, b.maxPrice, s.tickSize,
                                    intensity.data(), liquidity.data(), scale);
        if (r != HeatmapColumnStore::AppendResult::Written) {
            ++res.failed;
            continue;
        }
        ++res.written;
        (b.kind == Kind::Zero ? res.zero : res.recorded) += 1;
    }
    store.flush();
    return res;
}

} // namespace heatmap_fixture
