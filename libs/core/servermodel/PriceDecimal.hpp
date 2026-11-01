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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace recording {

// price == digits / 10^fraction exactly, with fraction minimal (no trailing zero
// digit after the point). exact == 0: no text was supplied, or the text is not a
// plain decimal "[0-9]+(.[0-9]+)?" with at most 17 significant digits and at most
// 63 fraction digits; its identity is then unknown and never guessed. With exact == 0,
// a nonzero `digits` is an UnsupportedPriceTexts id for diagnostics, never a value.
struct PriceDecimal {
    uint64_t digits : 57 = 0;
    uint64_t fraction : 6 = 0;
    uint64_t exact : 1 = 0;
};
static_assert(sizeof(PriceDecimal) == 8);

// The original text of prices that are not exact plain decimals, for diagnostics only:
// a bounded ring shared by the process. A producer records a text only for such a
// price (the supported path never allocates here) and keeps the returned id in
// PriceDecimal::digits; a consumer reads it back while the ring still holds it.
class UnsupportedPriceTexts {
  public:
    static constexpr size_t kCapacity = 1024;
    static constexpr size_t kMaxText = 256;
    static uint64_t record(std::string_view text) {
        auto &r = ring();
        std::lock_guard lock(r.mutex);
        const uint64_t id = ++r.next; // 1-based; < 2^57 for any realistic run
        auto &slot = r.entries[id % kCapacity];
        slot.id = id;
        slot.text.assign(text.substr(0, kMaxText));
        return id;
    }
    static std::optional<std::string> find(uint64_t id) {
        auto &r = ring();
        std::lock_guard lock(r.mutex);
        const auto &slot = r.entries[id % kCapacity];
        if (id == 0 || slot.id != id) return std::nullopt;
        return slot.text;
    }

  private:
    struct Slot {
        uint64_t id = 0;
        std::string text;
    };
    struct Ring {
        std::mutex mutex;
        uint64_t next = 0;
        std::array<Slot, kCapacity> entries;
    };
    static Ring &ring() {
        static Ring r;
        return r;
    }
};
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

// parsePriceDecimal, plus the text's ring id when it is not an exact plain decimal.
inline PriceDecimal priceDecimalWithText(std::string_view text) {
    PriceDecimal d = parsePriceDecimal(text);
    if (!d.exact) d.digits = UnsupportedPriceTexts::record(text) & ((uint64_t{1} << 57) - 1);
    return d;
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
