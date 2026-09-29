#pragma once
// Heatmap resolution policy (interaction spec docs/research/2026-09-heatmap-interaction-spec.md).
// - Timeframe: chosen by the user only. A column is exactly the timeframe; zoom never
//   changes it (there is no auto-timeframe). Time zoom-out clamps at one column per pixel.
// - Tick: Auto (smallest preset at least minRowPx tall, with hysteresis) or Manual
//   (a locked preset; never coarsens; price zoom-out clamps at one row per pixel).
// - Presets: {1, 2, 2.5, 5} x 10^k in price units, multiples of the common tick of
//   the data in view. All tick arithmetic is in integer price units
//   (price * priceScale), as recording::ladderTickUnits.
// Pure functions and value types; any thread.
#include "SparseColumns.hpp"
#include "../servermodel/PriceLadder.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heatmap {
// The smallest price-ladder tick that is at least minRowPx tall (no hysteresis).
// Defaults retain the BTC-USD near-grid policy; callers with other instruments
// supply their grid. The legacy page path (RecordingBandPolicy) uses this.
inline double idealTick(double minPrice, double maxPrice, double heightPx, double minRowPx,
                        double finestTick = 1.0, double priceScale = 100.0) {
    if (!std::isfinite(minPrice) || !std::isfinite(maxPrice) || maxPrice <= minPrice ||
        !std::isfinite(heightPx) || heightPx <= 0 || !std::isfinite(minRowPx) || minRowPx <= 0 ||
        !std::isfinite(finestTick) || finestTick <= 0 || !std::isfinite(priceScale) || priceScale <= 0 ||
        finestTick * priceScale >= 0x1p63) return 0;
    return recording::ladderTick((maxPrice - minPrice) / heightPx * minRowPx, finestTick, priceScale);
}
// Migration only (near/deep HMC2 layers); not a product rule.
inline std::string_view layerFor(double tick, double deepTick, int64_t tfMs) {
    return tfMs >= kHourMs || tick >= deepTick ? "deep" : "near";
}

// ------------------------------------------------------------------ presets
enum class TickMode : uint8_t { Auto, Manual };

// Every preset {1, 2, 2.5, 5} x 10^k that is a whole number of price units, ascending.
inline const std::vector<int64_t> &presetLadderUnits() {
    static const std::vector<int64_t> ladder = [] {
        std::vector<int64_t> out;
        static constexpr int64_t kMantissaTenths[] = {10, 20, 25, 50};
        int64_t decade = 1;
        for (int k = 0; k < 19; ++k) {
            for (const int64_t m : kMantissaTenths) {
                if (decade > std::numeric_limits<int64_t>::max() / m) return out;
                const int64_t tenths = m * decade;
                if (tenths % 10 == 0) out.push_back(tenths / 10);
            }
            if (decade > std::numeric_limits<int64_t>::max() / 10) break;
            decade *= 10;
        }
        return out;
    }();
    return ladder;
}
inline bool isPresetUnits(int64_t units) {
    const auto &ladder = presetLadderUnits();
    return std::binary_search(ladder.begin(), ladder.end(), units);
}
// True when a display tick can be built from a native grid (or set of grids)
// whose common tick is commonUnits: the tick is a whole multiple of it.
inline bool buildsOn(int64_t tickUnits, int64_t commonUnits) {
    return tickUnits > 0 && commonUnits > 0 && tickUnits % commonUnits == 0;
}
// Smallest preset >= minUnits that is a multiple of commonUnits; 0 if none.
inline int64_t presetAtLeast(double minUnits, int64_t commonUnits) {
    if (commonUnits <= 0 || std::isnan(minUnits)) return 0;
    for (const int64_t p : presetLadderUnits())
        if (double(p) >= minUnits && p % commonUnits == 0) return p;
    return 0;
}
// Price <-> integer price units. 0 for non-positive or non-finite input.
inline int64_t toUnits(double price, double priceScale) {
    const double units = price * priceScale;
    if (!std::isfinite(units) || units <= 0 || units >= 0x1p62) return 0;
    return int64_t(std::llround(units));
}
inline double fromUnits(int64_t units, double priceScale) {
    return priceScale > 0 ? double(units) / priceScale : 0;
}

// ------------------------------------------------------------------ Auto
struct AutoTickParams {
    double minRowPx = 2;      // physical pixels
    double hysteresis = 0.25; // h, clamped to [0, 0.9]
};

// Auto tick, stateful: `currentUnits` is the tick drawn now (0 = none yet, which
// evaluates fresh). `unitsPerPx` is price units per physical pixel of the view.
// Rows of tick T are T / unitsPerPx pixels tall.
// - Target: the smallest preset (multiple of commonUnits) whose rows are >= minRowPx.
// - Step finer only to a preset whose rows are >= minRowPx * (1 + h).
// - Step coarser only when the current rows fall below minRowPx * (1 - h); then
//   take the target. A current tick the data in view cannot build is replaced by
//   the target at once (Auto may skip to the finest compatible preset).
// With h = 0 this is the plain target (idealTick). For h > 0 the result is a fixed
// point: re-evaluating at the same zoom never changes it, and a zoom that stays
// inside the hysteresis band around a threshold never flips it.
inline int64_t autoTickUnits(int64_t currentUnits, int64_t commonUnits, double unitsPerPx,
                             const AutoTickParams &params = {}) {
    if (commonUnits <= 0 || !std::isfinite(unitsPerPx) || !(unitsPerPx > 0) || !std::isfinite(params.minRowPx) ||
        !(params.minRowPx > 0))
        return 0;
    const double h = std::isfinite(params.hysteresis) ? std::clamp(params.hysteresis, 0.0, 0.9) : 0.0;
    const int64_t target = presetAtLeast(params.minRowPx * unitsPerPx, commonUnits);
    if (!buildsOn(currentUnits, commonUnits) || !isPresetUnits(currentUnits)) return target;
    const double rowPx = double(currentUnits) / unitsPerPx;
    if (rowPx < params.minRowPx * (1 - h)) return target; // coarser
    const int64_t finer = presetAtLeast(params.minRowPx * (1 + h) * unitsPerPx, commonUnits);
    if (finer > 0 && finer < currentUnits) return finer;
    return currentUnits;
}

// ------------------------------------------------------------------ Manual
// Presets offered to Manual, ascending, at most maxUnits: every preset that at
// least one of the loaded native grids (given by their common ticks) can build.
// A preset some of the data cannot build is still offered; those regions draw
// the veil and the UI shows the resolution indicator (spec rule 2).
inline std::vector<int64_t> manualPresetUnits(const std::vector<int64_t> &gridCommonUnits, int64_t maxUnits) {
    std::vector<int64_t> out;
    for (const int64_t p : presetLadderUnits()) {
        if (p > maxUnits) break;
        for (const int64_t c : gridCommonUnits)
            if (buildsOn(p, c)) { out.push_back(p); break; }
    }
    return out;
}

// ------------------------------------------------------------------ clamps
// Time zoom-out stops at one column per physical pixel (spec rule 1).
inline double maxTimeSpanMs(double widthPx, int64_t tfMs) {
    return std::isfinite(widthPx) && widthPx > 0 && tfMs > 0 ? widthPx * double(tfMs) : 0;
}
// Manual price zoom-out stops at one row per physical pixel (spec rule 2).
inline double maxManualPriceSpan(double heightPx, double tick) {
    return std::isfinite(heightPx) && heightPx > 0 && std::isfinite(tick) && tick > 0 ? heightPx * tick : 0;
}
// Shrinks [lo, hi) about its centre so hi - lo <= maxSpan (no-op if maxSpan <= 0).
inline void clampSpan(double &lo, double &hi, double maxSpan) {
    if (!(maxSpan > 0) || !(hi - lo > maxSpan)) return;
    const double centre = (lo + hi) * 0.5;
    lo = centre - maxSpan * 0.5;
    hi = centre + maxSpan * 0.5;
}

// The Manual tick per (symbol, timeframe), so BTC 1m can keep $1 while BTC 1h
// keeps $25 (spec rule 2). Units are integer price units of the symbol's grid.
class ManualTickMemory {
public:
    std::optional<int64_t> get(const std::string &symbol, int64_t tfMs) const {
        const auto it = ticks_.find({symbol, tfMs});
        return it == ticks_.end() ? std::nullopt : std::optional<int64_t>(it->second);
    }
    // Stores a preset; returns false (and stores nothing) for a non-preset.
    bool set(const std::string &symbol, int64_t tfMs, int64_t units) {
        if (symbol.empty() || tfMs <= 0 || !isPresetUnits(units)) return false;
        ticks_[{symbol, tfMs}] = units;
        return true;
    }
    void erase(const std::string &symbol, int64_t tfMs) { ticks_.erase({symbol, tfMs}); }
    const std::map<std::pair<std::string, int64_t>, int64_t> &entries() const { return ticks_; }
private:
    std::map<std::pair<std::string, int64_t>, int64_t> ticks_;
};
} // namespace heatmap
