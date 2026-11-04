/*
Sentinel — PriceDecimal
Role: exact decimal identity of an exchange price, taken from its text before any
binary64 conversion. The recorder keeps keying levels on llround(double * scale);
this is an additive judgement that says whether that key IS the decimal.
Threading: pure functions, any thread.
*/
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace recording {

// price == digits / 10^fraction exactly, with fraction minimal (no trailing zero
// digit after the point). exact == 0: no text was supplied, or the text is not a
// plain decimal "[0-9]+(.[0-9]+)?" with at most 17 significant digits and at most
// 63 fraction digits; its identity is then unknown and never guessed.
struct PriceDecimal {
    uint64_t digits : 57 = 0;
    uint64_t fraction : 6 = 0;
    uint64_t exact : 1 = 0;
};
static_assert(sizeof(PriceDecimal) == 8);

// The original text of one price in a message that is not an exact plain decimal,
// for diagnostics only: the producer fills it for the first such price of a message
// (the only allocation; supported prices never allocate) and the recorder message owns
// it until the worker has processed that message. `level` indexes the message's levels.
struct PriceText {
    uint32_t level = UINT32_MAX;
    std::string text;
};
// The text kept for diagnostics: whole up to kMaxPriceText, else its head and tail
// with an explicit marker and the original length (the distinguishing suffix stays).
inline constexpr size_t kMaxPriceText = 256;
inline std::string diagnosticPriceText(std::string_view text) {
    if (text.size() <= kMaxPriceText) return std::string(text);
    constexpr size_t keep = kMaxPriceText / 2 - 24;
    return std::string(text.substr(0, keep)) + "...[" + std::to_string(text.size()) + " chars, middle omitted]..." +
           std::string(text.substr(text.size() - keep));
}

inline constexpr int kMaxPriceDigits = 17; // 10^17 - 1 < 2^57
inline constexpr int kMaxScaleDigits = 18; // 10^18 < 2^63

inline constexpr std::array<int64_t, kMaxScaleDigits + 1> kPow10 = [] {
    std::array<int64_t, kMaxScaleDigits + 1> p{};
    p[0] = 1;
    for (int i = 1; i <= kMaxScaleDigits; ++i) p[i] = p[i - 1] * 10;
    return p;
}();

inline PriceDecimal parsePriceDecimal(std::string_view text) noexcept {
    const size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || (dot != std::string_view::npos && frac.empty())) return {};
    for (const std::string_view part : {whole, frac})
        for (const char c : part)
            if (c < '0' || c > '9') return {}; // also rejects sign, exponent, spaces and a second '.'
    // Trailing fraction zeros carry no identity; leading zeros are not significant.
    while (!frac.empty() && frac.back() == '0') frac.remove_suffix(1);
    if (frac.size() > 63) return {};
    uint64_t value = 0;
    int significant = 0;
    for (const std::string_view part : {whole, frac})
        for (const char c : part) {
            if (significant == 0 && c == '0') continue;
            if (++significant > kMaxPriceDigits) return {};
            value = value * 10 + uint64_t(c - '0');
        }
    return {value, uint64_t(frac.size()), 1}; // zero parses as exact 0; the recorder rejects it
}

// k when scale == 10^k exactly (0 <= k <= 18), else -1. A non-decimal scale has
// no exact decimal grid: every price is then unsupported for exact consumers.
inline int priceScaleDigits(double scale) noexcept {
    for (int k = 0; k <= kMaxScaleDigits; ++k)
        if (scale == static_cast<double>(kPow10[k])) return k;
    return -1;
}

// Exact identity of a decimal on the 10^scaleDigits grid against the recorder's key.
//   Exact:    the text is a supported decimal, lies on the grid, and its units
//             equal the recorder's key (llround(price * scale)).
//   NoText:   no text, a text that is not a supported plain decimal, or a scale
//             that is not 10^k.
//   Finer:    more fraction digits than the product scale (sub-tick price or a
//             tick-size change to a finer tick).
//   Mismatch: on the grid but outside the domain: the units overflow int64, or
//             the recorder's double key differs from the exact units (the double
//             product rounded to another level; impossible below 2^51 units).
enum class DecimalIdentity : uint8_t { Exact, NoText, Finer, Mismatch };
inline const char *identityName(DecimalIdentity i) noexcept {
    switch (i) {
    case DecimalIdentity::Exact: return "exact";
    case DecimalIdentity::NoText: return "no-decimal-text";
    case DecimalIdentity::Finer: return "finer-than-scale";
    case DecimalIdentity::Mismatch: return "outside-domain";
    }
    return "?";
}
inline std::optional<int64_t> decimalUnits(PriceDecimal d, int scaleDigits) noexcept {
    if (!d.exact || scaleDigits < 0 || scaleDigits > kMaxScaleDigits || int(d.fraction) > scaleDigits) return {};
    const int64_t mul = kPow10[size_t(scaleDigits - int(d.fraction))];
    if (d.digits > uint64_t(std::numeric_limits<int64_t>::max() / mul)) return {};
    return int64_t(d.digits) * mul;
}
inline DecimalIdentity decimalIdentity(PriceDecimal d, int scaleDigits, int64_t recorderUnits) noexcept {
    if (!d.exact || scaleDigits < 0) return DecimalIdentity::NoText;
    if (int(d.fraction) > scaleDigits) return DecimalIdentity::Finer;
    const auto units = decimalUnits(d, scaleDigits);
    return units && *units == recorderUnits ? DecimalIdentity::Exact : DecimalIdentity::Mismatch;
}

} // namespace recording
