#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace chart {
// Magnitudes use at most four characters plus one suffix. A signed consumer
// may prepend one minus sign. Values beyond the B range use >999B.
inline constexpr int kFlowMagnitudeGlyphs = 5;
namespace flow_format {
inline uint64_t power10(int exponent) {
    uint64_t result = 1;
    while (exponent-- > 0) result *= 10;
    return result;
}
// Parse the shortest decimal representation before rounding. Integer division
// implements decimal half-up without binary multiply/divide errors at ties.
inline uint64_t roundedCents(double value) {
    char decimal[64];
    const auto end = std::to_chars(decimal, decimal + sizeof(decimal), value, std::chars_format::fixed).ptr;
    uint64_t coefficient = 0;
    int fraction = 0, whole = 0;
    bool afterPoint = false;
    for (auto* p = decimal; p != end; ++p) {
        if (*p == '.') { afterPoint = true; continue; }
        coefficient = coefficient * 10 + unsigned(*p - '0');
        if (afterPoint) ++fraction; else ++whole;
    }
    const int exponent = value < 10 ? -2 : value < 100 ? -1 : whole - 3;
    const int discarded = fraction + exponent;
    if (discarded > 0) {
        const auto divisor = power10(discarded);
        coefficient = (coefficient + divisor / 2) / divisor;
    } else coefficient *= power10(-discarded);
    return coefficient * power10(exponent + 2);
}
inline char* fixed(uint64_t amount, int precision, bool trim, char* out) {
    const auto divisor = power10(precision);
    char* begin = out;
    if (amount >= divisor) out = std::to_chars(out, out + 20, amount / divisor).ptr;
    if (precision) {
        *out++ = '.';
        auto remainder = amount % divisor;
        for (auto place = divisor / 10; place; place /= 10) {
            *out++ = char('0' + remainder / place); remainder %= place;
        }
        if (trim) {
            while (out > begin && out[-1] == '0') --out;
            if (out > begin && out[-1] == '.') --out;
        }
    }
    return out;
}
inline char* magnitude(double value, char* out) {
    const auto cents = value < 1e12 ? roundedCents(value) : uint64_t(100000000000000);
    if (cents >= 100000000000000) {
        for (char c : {'>', '9', '9', '9', 'B'}) *out++ = c;
        return out;
    }
    if (cents < 100000) {
        const int precision = cents < 1000 ? 2 : cents < 10000 ? 1 : 0;
        return fixed(cents / power10(2 - precision), precision, false, out);
    }
    const int unit = cents >= 100000000000 ? 3 : cents >= 100000000 ? 2 : 1;
    out = fixed(cents / power10(3 * unit), 2, true, out);
    *out++ = "kMB"[unit - 1];
    return out;
}
} // namespace flow_format
enum class FlowNumberContext { Footprint, SignedLabel };
inline int flowNumber(double value, char (&out)[24], FlowNumberContext context = FlowNumberContext::Footprint) {
    out[0] = '\0';
    if (!std::isfinite(value)) return 0;
    if (context == FlowNumberContext::SignedLabel && std::abs(value) < .01) {
        out[0] = '0'; out[1] = '\0'; return 1;
    }
    if (value == 0) return 0;
    char* begin = out;
    if (value < 0) { *begin++ = '-'; value = -value; }
    char* end = begin;
    if (value < .01) {
        for (char c : {'<', '.', '0', '1'}) *end++ = c;
    } else end = flow_format::magnitude(value, begin);
    *end = '\0'; return int(end - out);
}
enum class FlowTier { Empty, Dust, Normal, Heavy };
inline FlowTier flowTier(double volume, double price, double maximum, double dustNotional) {
    if (!std::isfinite(volume) || volume <= 0) return FlowTier::Empty;
    if (volume < .01 || volume * price < dustNotional) return FlowTier::Dust;
    return volume > .55 * maximum ? FlowTier::Heavy : FlowTier::Normal;
}
inline double flowTextAlpha(FlowTier tier) {
    return tier == FlowTier::Dust ? .22 : tier == FlowTier::Heavy ? 1 : tier == FlowTier::Normal ? .6 : 0;
}
} // namespace chart
