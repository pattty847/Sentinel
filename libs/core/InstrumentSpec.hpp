#pragma once
// RAWL2 instrument identity and exact decimal metadata. Missing venue means
// Coinbase; an explicit venue is currently only "kalshi". Coinbase archives
// have no price bounds: retain their increment strings without tightening the
// historical header rules. Kalshi bands are ordered, disjoint intervals;
// adjacent endpoints are allowed. This describes metadata, not a trading grid.
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sentinel {
enum class Venue { Coinbase, Kalshi };

inline Venue venueFromHeader(const nlohmann::json& header) {
    if (!header.contains("venue")) return Venue::Coinbase;
    if (header.at("venue") == "kalshi") return Venue::Kalshi;
    throw std::runtime_error("RAWL2: unsupported venue");
}

inline void validateInstrumentId(const std::string& id, Venue venue) {
    if (venue != Venue::Coinbase && venue != Venue::Kalshi)
        throw std::runtime_error("RAWL2: unsupported venue");
    const bool kalshi = venue == Venue::Kalshi;
    if (id.empty() || id.size() > (kalshi ? 64 : 40) ||
        id.find_first_not_of(kalshi ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-." :
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos ||
        (kalshi && (id.front() == '.' || id.find("..") != std::string::npos)))
        throw std::runtime_error("RAWL2: invalid symbol");
}

struct InstrumentSpec {
    struct Bounds { std::string min, max; };
    struct TickBand {
        // Unbounded only for Coinbase's single increment.
        std::optional<Bounds> bounds;
        std::string step;
    };
    Venue venue = Venue::Coinbase;
    std::string id;
    std::optional<Bounds> priceBounds;
    std::vector<TickBand> tickBands;
    std::string sizeIncrement;

    static InstrumentSpec fromHeader(const nlohmann::json& header);
};

namespace instrument_detail {
// Compare normalized plain decimals as strings: no rounding, scale conversion
// or integer overflow. Original strings remain in InstrumentSpec and the file.
inline std::string decimal(const nlohmann::json& value) {
    if (!value.is_string()) throw std::runtime_error("RAWL2: instrument decimal must be a string");
    auto s = value.get<std::string>();
    const auto dot = s.find('.');
    if (s.empty() || s.size() > 80 || dot == 0 || dot == s.size() - 1 ||
        s.find_first_not_of("0123456789.") != std::string::npos ||
        (dot != std::string::npos && s.find('.', dot + 1) != std::string::npos))
        throw std::runtime_error("RAWL2: invalid instrument decimal");
    const auto first = s.find_first_not_of('0');
    s.erase(0, first == std::string::npos ? s.size() - 1 : first);
    if (s.front() == '.') s.insert(0, 1, '0');
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}
inline int compare(const std::string& a, const std::string& b) {
    const auto whole = [](const auto& s) { const auto d = s.find('.'); return d == std::string::npos ? s.size() : d; };
    if (whole(a) != whole(b)) return whole(a) < whole(b) ? -1 : 1;
    return a.compare(b);
}
}

inline InstrumentSpec InstrumentSpec::fromHeader(const nlohmann::json& header) {
    InstrumentSpec spec;
    spec.venue = venueFromHeader(header);
    const auto& product = header.at("product_metadata");
    spec.id = product.at("product_id").get<std::string>();
    validateInstrumentId(spec.id, spec.venue);
    if (spec.venue == Venue::Coinbase) {
        if (!product.at("quote_increment").is_string() || !product.at("base_increment").is_string())
            throw std::runtime_error("RAWL2: increments must be strings");
        spec.tickBands.push_back({{}, product.at("quote_increment").get<std::string>()});
        spec.sizeIncrement = product.at("base_increment").get<std::string>();
        return spec;
    }
    if (!header.contains("instrument") || !header.at("instrument").is_object())
        throw std::runtime_error("RAWL2: missing or malformed instrument block");
    const auto& instrument = header.at("instrument");
    using instrument_detail::decimal;
    using instrument_detail::compare;
    const auto low = decimal(instrument.at("price_min"));
    const auto high = decimal(instrument.at("price_max"));
    if (compare(low, high) >= 0 || compare(high, "1") > 0)
        throw std::runtime_error("RAWL2: invalid Kalshi price bounds");
    spec.priceBounds = Bounds{instrument.at("price_min"), instrument.at("price_max")};
    if (decimal(instrument.at("size_increment")) == "0")
        throw std::runtime_error("RAWL2: size increment must be positive");
    spec.sizeIncrement = instrument.at("size_increment");
    const auto& bands = instrument.at("price_ranges");
    if (!bands.is_array() || bands.empty()) throw std::runtime_error("RAWL2: missing price bands");
    auto previous = low;
    for (const auto& band : bands) {
        const auto start = decimal(band.at("start"));
        const auto end = decimal(band.at("end"));
        if (decimal(band.at("step")) == "0") throw std::runtime_error("RAWL2: price step must be positive");
        if (compare(start, end) >= 0 || compare(start, previous) < 0 || compare(end, high) > 0)
            throw std::runtime_error("RAWL2: unordered, overlapping or out-of-bounds price band");
        spec.tickBands.push_back({Bounds{band.at("start"), band.at("end")}, band.at("step")});
        previous = end;
    }
    return spec;
}
} // namespace sentinel
