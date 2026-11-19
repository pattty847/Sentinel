#pragma once
// The one number format of chart canvas text: heatmap cell labels, footprint cell
// numbers, the flow footer, the CVD gutter values and the wall badges all print a
// value through formatChartNumber, so the same value reads the same in every layer
// (brief principle 8). Qt-free and allocation-free (render thread, per label).
// - Full form: three significant digits; Compact: two, but never fewer than the
//   integer digits (123k stays 123k, 12.3k becomes 12k). A layer steps all its labels
//   to Compact at once before it hides its text (ChartTextRule.hpp).
// - Trailing zeros are dropped ($11k, $1.2M, 0.5: owner 2026-10-02); the leading zero
//   is kept (0.36). k/M/B/T from 1,000 (999.6 -> 1k); beyond 999T: >999T.
// - Below 1, at most three decimals; a nonzero value that rounds to zero prints
//   <0.001, never 0. Zero prints 0. Never an exponent; non-finite prints nothing.
// - Rounding is decimal half-up on the shortest decimal form of the double, so
//   1.005 -> 1.01 and 0.995 -> 1, as a person rounds the printed value.
// - '-' for negatives; NumberStyle::sign adds '+' to positives (the footer's DELTA and
//   CUM rows); NumberStyle::currency adds '$' after the sign (and after '<' or '>').
// - Same value and same form give the same text in every layer. Layers decide their form
//   on their own (one may print Full while another prints Compact).
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chart {
enum class NumberForm : uint8_t { Full, Compact };
inline constexpr int kNumberForms = 2;
struct NumberStyle {
    NumberForm form = NumberForm::Full;
    bool sign = false, currency = false;
};
// Every magnitude form of a value of at least 0.001 fits in this many characters
// ("1.23k", "0.123", ">999T"), before a sign or a currency symbol.
inline constexpr int kFlowMagnitudeGlyphs = 5;
namespace number_format {
inline uint64_t power10(int exponent) {
    uint64_t result = 1;
    while (exponent-- > 0) result *= 10;
    return result;
}
// value = digits * 10^exponent, from the shortest round-trip decimal of a positive
// double; order = floor(log10(value)).
struct Decimal { uint64_t digits = 0; int exponent = 0, order = 0; };
inline Decimal shortest(double value) {
    char text[48];
    const auto end = std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific).ptr;
    Decimal out;
    int fraction = 0;
    bool afterPoint = false;
    const char* p = text;
    for (; p != end && *p != 'e'; ++p) {
        if (*p == '.') { afterPoint = true; continue; }
        out.digits = out.digits * 10 + unsigned(*p - '0');
        fraction += afterPoint;
    }
    int exponent = 0;
    if (p != end) std::from_chars(p + 1 + (p[1] == '+'), end, exponent);
    out.exponent = exponent - fraction;
    out.order = exponent;
    return out;
}
// round(value * 10^shift), decimal half-up.
inline uint64_t scaled(const Decimal& d, int shift) {
    if (shift >= 0) return shift > 18 ? UINT64_MAX : d.digits * power10(shift);
    if (shift < -19) return 0;
    const uint64_t divisor = power10(-shift), remainder = d.digits % divisor;
    return d.digits / divisor + (remainder >= divisor - remainder ? 1 : 0);
}
struct Rounded { uint64_t amount = 0; int decimals = 0, unit = 0; bool overflow = false; };
// The rounded amount in units of 10^(3 * unit). A carry adds an integer digit (9.996 -> 10)
// or moves to the next unit (999.6 -> 1k).
inline Rounded round(const Decimal& d, int significant) {
    int unit = 0, order = d.order;
    for (int pass = 0; pass < 8; ++pass) {
        if (order >= 3) {
            if (unit == 4) return {0, 0, 4, true};
            ++unit;
            order -= 3;
            continue;
        }
        int decimals = std::max(0, significant - 1 - order);
        if (order < 0) decimals = std::min(decimals, 3);
        const uint64_t amount = scaled(d, d.exponent - 3 * unit + decimals);
        if (amount < power10(order + 1 + decimals)) return {amount, decimals, unit};
        ++order;
    }
    return {0, 0, unit, true};
}
inline char* write(char* out, const Rounded& r) {
    const uint64_t divisor = power10(r.decimals);
    out = std::to_chars(out, out + 20, r.amount / divisor).ptr;
    if (r.decimals) {
        char* point = out;
        *out++ = '.';
        uint64_t remainder = r.amount % divisor;
        for (uint64_t place = divisor / 10; place; place /= 10) {
            *out++ = char('0' + remainder / place);
            remainder %= place;
        }
        while (out[-1] == '0') --out;
        if (out == point + 1) out = point;
    }
    if (r.unit) *out++ = "kMBT"[r.unit - 1];
    return out;
}
} // namespace number_format
// Writes the text of a nonzero finite value from its shortest decimal (number_format::shortest
// of |value|) and its terminator; returns the length.
inline int formatChartNumber(bool negative, const number_format::Decimal& decimal, char (&out)[24],
                             NumberStyle style = {}) {
    char* p = out;
    if (negative) *p++ = '-';
    else if (style.sign) *p++ = '+';
    const auto currency = [&] { if (style.currency) *p++ = '$'; };
    const auto rounded = number_format::round(decimal, style.form == NumberForm::Full ? 3 : 2);
    if (rounded.overflow) {
        *p++ = '>';
        currency();
        for (char c : {'9', '9', '9', 'T'}) *p++ = c;
    } else if (rounded.amount == 0) {
        *p++ = '<';
        currency();
        for (char c : {'0', '.', '0', '0', '1'}) *p++ = c;
    } else {
        currency();
        p = number_format::write(p, rounded);
    }
    *p = '\0';
    return int(p - out);
}
// Writes the text of `value` and its terminator; returns the length (0 for non-finite).
inline int formatChartNumber(double value, char (&out)[24], NumberStyle style = {}) {
    out[0] = '\0';
    if (!std::isfinite(value)) return 0;
    if (value == 0) {
        char* p = out;
        if (style.currency) *p++ = '$';
        *p++ = '0';
        *p = '\0';
        return int(p - out);
    }
    return formatChartNumber(value < 0, number_format::shortest(std::abs(value)), out, style);
}
enum class FlowTier { Empty, Dust, Normal, Heavy };
inline FlowTier flowTier(double volume, double price, double maximum, double dustNotional) {
    if (!std::isfinite(volume) || volume <= 0) return FlowTier::Empty;
    if (volume < .01 || volume * price < dustNotional) return FlowTier::Dust;
    return volume > .55 * maximum ? FlowTier::Heavy : FlowTier::Normal;
}
inline double flowTextAlpha(FlowTier tier) {
    return tier == FlowTier::Heavy ? 1 : tier == FlowTier::Normal ? .6 : 0;
}
} // namespace chart
