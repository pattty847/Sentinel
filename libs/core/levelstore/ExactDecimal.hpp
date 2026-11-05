#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sentinel::levelstore {

// Nonnegative plain decimal: [0-9]+(\.[0-9]+)?. Signs, whitespace and exponent
// notation are invalid. Leading zeros and trailing fractional zeros are allowed.
// No text-length truncation: validate the entire input before reporting overflow.
// Value = mantissa * 10^-digits; trailing fractional zeros are canonicalized away.
// An int64 mantissa overflow or a fraction longer than UINT32_MAX is a range
// failure on a valid decimal, never malformed input or a market-data gap.
enum class DecimalStatus { Exact, Invalid, Overflow };
struct ExactDecimal {
    int64_t mantissa = 0;
    uint32_t digits = 0;
    bool operator==(const ExactDecimal &) const = default;
};
struct DecimalResult {
    ExactDecimal value;
    DecimalStatus status = DecimalStatus::Invalid;
};

inline DecimalResult parseDecimal(std::string_view text) noexcept {
    const auto dot = text.find('.');
    auto whole = text.substr(0, dot);
    auto fraction = dot == text.npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || (dot != text.npos && fraction.empty())) return {};
    for (const auto part : {whole, fraction})
        for (const char c : part)
            if (c < '0' || c > '9') return {};
    while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
    if (fraction.size() > UINT32_MAX) return {{}, DecimalStatus::Overflow};
    int64_t n = 0;
    for (const auto part : {whole, fraction}) {
        for (const char c : part) {
            const int digit = c - '0';
            if (n > (INT64_MAX - digit) / 10) return {{}, DecimalStatus::Overflow};
            n = n * 10 + digit;
        }
    }
    return {{n, static_cast<uint32_t>(fraction.size())}, DecimalStatus::Exact};
}

// Changing scale never rounds. A coarser target must divide exactly; a finer
// target must fit int64. Zero is representable at every scale.
inline std::optional<int64_t> decimalAtScale(ExactDecimal value, uint32_t digits) noexcept {
    if (value.mantissa < 0) return {};
    if (value.mantissa == 0) return 0;
    while (value.digits < digits) {
        if (value.mantissa > INT64_MAX / 10) return {};
        value.mantissa *= 10;
        ++value.digits;
    }
    while (value.digits > digits) {
        if (value.mantissa % 10 != 0) return {};
        value.mantissa /= 10;
        --value.digits;
    }
    return value.mantissa;
}

inline std::string decimalText(ExactDecimal value) {
    std::string out = std::to_string(value.mantissa);
    if (value.digits) {
        if (out.size() <= value.digits) out.insert(0, size_t(value.digits) + 1 - out.size(), '0');
        out.insert(out.size() - value.digits, 1, '.');
    }
    return out;
}

// A non-power-of-ten increment (e.g. 0.05) uses decimal units (0.01), and
// divisibility by its mantissa determines grid membership. No binary arithmetic.
inline bool onDecimalGrid(ExactDecimal value, ExactDecimal increment) noexcept {
    if (value.mantissa < 0 || increment.mantissa <= 0) return false;
    if (value.digits >= increment.digits) {
        const auto units = decimalAtScale(value, increment.digits);
        return units && *units % increment.mantissa == 0;
    }
    const auto modulus = static_cast<uint64_t>(increment.mantissa);
    uint64_t remainder = static_cast<uint64_t>(value.mantissa) % modulus;
    while (value.digits++ < increment.digits && remainder) {
        const uint64_t twice = (remainder * 2) % modulus;
        const uint64_t eight = (((twice * 2) % modulus) * 2) % modulus;
        remainder = (eight + twice) % modulus;
    }
    return remainder == 0;
}

} // namespace sentinel::levelstore
