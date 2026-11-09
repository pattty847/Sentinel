/*
Sentinel — PriceDisplayUnit
Role: the scaled unit a tiny-priced product's prices are shown in (owner decision
2026-11-08, option A): PEPE at 0.00000504 shows as 5.04 with the tag "x1M". The unit
is a power of 1000 that brings the reference price into [1, 1000); it applies only
below kScaledBelowPrice, so BTC, ETH, SOL, AVAX, DOGE and FARTCOIN stay in plain
quote currency. Display only: stored, wire and Agent API prices stay raw.
Threading: pure functions, any thread. No Qt.
*/
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace sentinel {

struct PriceDisplayUnit {
    int exponent = 0; // shown = raw * 10^exponent: 0 (plain), 3, 6, 9 or 12

    bool scaled() const { return exponent > 0; }
    double factor() const { return std::pow(10.0, exponent); }
    double toDisplay(double raw) const { return scaled() ? raw * factor() : raw; }
    double toRaw(double shown) const { return scaled() ? shown / factor() : shown; }
    // "x1K", "x1M", "x1B", "x1T"; empty when plain.
    const char *tag() const {
        switch (exponent) {
        case 3: return "x1K";
        case 6: return "x1M";
        case 9: return "x1B";
        case 12: return "x1T";
        default: return "";
        }
    }
    bool operator==(const PriceDisplayUnit &) const = default;
};

// A product whose price is below this is shown scaled.
inline constexpr double kScaledBelowPrice = 0.01;
inline constexpr int kMaxDisplayExponent = 12;

// The unit for a product trading near `referencePrice` (book mid or last trade).
// Plain for a price at or above kScaledBelowPrice, or an unknown (non-positive or
// non-finite) price. Otherwise the smallest power of 1000 that lifts the price to at
// least 1 (so into [1, 1000)), at most 10^12.
inline PriceDisplayUnit choosePriceDisplayUnit(double referencePrice) {
    if (!std::isfinite(referencePrice) || !(referencePrice > 0) || referencePrice >= kScaledBelowPrice) return {};
    PriceDisplayUnit unit;
    double shown = referencePrice;
    // 1e-9 relative slack: 0.001 * 1000 must count as 1, not 0.999999.
    while (unit.exponent < kMaxDisplayExponent && shown < 1.0 - 1e-9) {
        unit.exponent += 3;
        shown = referencePrice * unit.factor();
    }
    return unit;
}

// Decimals that show a step of `displayStep` without rounding it (0..12); 0 for a
// whole or invalid step. 0.01 -> 2, 0.5 -> 1, 2.5 -> 1, 10 -> 0.
inline int displayDecimals(double displayStep) {
    if (!std::isfinite(displayStep) || !(displayStep > 0)) return 0;
    int decimals = 0;
    double scaled = displayStep;
    while (decimals < 12 && std::abs(scaled - std::round(scaled)) > 1e-9 * std::max(1.0, scaled)) {
        scaled *= 10;
        ++decimals;
    }
    return decimals;
}

// `raw` in `unit`, with the decimals of `rawStep` (e.g. the tick) in that unit:
// (0.00000504, 1e-8, x1M) -> "5.04". No currency sign or tag.
inline std::string formatDisplayPrice(double raw, double rawStep, PriceDisplayUnit unit) {
    char text[64];
    const int decimals = displayDecimals(unit.toDisplay(rawStep));
    std::snprintf(text, sizeof text, "%.*f", decimals, unit.toDisplay(raw));
    return text;
}

} // namespace sentinel
