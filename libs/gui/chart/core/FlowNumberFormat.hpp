#pragma once
#include <algorithm>
#include <charconv>
#include <cmath>

namespace chart {
// Magnitudes use at most four characters plus one suffix. A signed consumer
// may prepend one minus sign. Values beyond the B range use >999B.
inline constexpr int kFlowMagnitudeGlyphs = 5;
namespace flow_format {
inline int decimals(double value) { return value < 10 ? 2 : value < 100 ? 1 : 0; }
inline char scale(double& value) {
    if (value < 1000) return 0;
    char suffix = 0;
    for (char c : {'k', 'M', 'B'}) {
        value /= 1000; suffix = c;
        if (value < 1000) break;
    }
    value = std::min(value, 999.0);
    return suffix;
}
inline char* magnitude(double value, char* begin) {
    if (value >= 1e12) {
        for (char c : {'>', '9', '9', '9', 'B'}) *begin++ = c;
        return begin;
    }
    const char suffix = scale(value);
    int precision = decimals(value);
    const double factor = precision == 2 ? 100 : precision == 1 ? 10 : 1;
    value = (suffix ? std::floor(value * factor) : std::round(value * factor)) / factor;
    precision = decimals(value);
    char* end = std::to_chars(begin, begin + 20, value, std::chars_format::fixed, precision).ptr;
    if (value < 1) { for (char* p = begin; p + 1 < end; ++p) *p = *(p + 1); --end; }
    if (suffix && precision) {
        while (end > begin && end[-1] == '0') --end;
        if (end > begin && end[-1] == '.') --end;
    }
    if (suffix) *end++ = suffix;
    return end;
}
} // namespace flow_format
inline int flowNumber(double value, char (&out)[24]) {
    out[0] = '\0';
    if (!std::isfinite(value) || value == 0) return 0;
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
