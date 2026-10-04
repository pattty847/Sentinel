#include "Grid.hpp"
#include <cmath>
#include <stdexcept>

namespace sentinel::roller {
namespace {
double decimal(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789.") != std::string::npos)
        throw std::runtime_error("metadata increment must be a positive decimal");
    size_t used = 0;
    const auto value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value) || value <= 0) throw std::runtime_error("invalid increment");
    return value;
}
double next125(double tick) {
    const auto decade = std::pow(10.0, std::floor(std::log10(tick)));
    // Native-increment rounding can put near between ladder rungs. Count the
    // next strictly higher rung; tolerate floating-point noise at an exact rung.
    for (double m : {1.,2.,5.,10.,20.}) {
        const auto next = m * decade;
        if (next > tick * (1.0 + 1e-12)) return next;
    }
    throw std::runtime_error("unrepresentable deep tick");
}
double scaleFor(std::string text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos) return 1;
    while (text.back() == '0') text.pop_back();
    const auto places = text.size() - dot - 1;
    if (places > 15) throw std::runtime_error("quote increment exceeds binary64 price grid precision");
    return std::pow(10.0, double(places));
}
}
Grid deriveGrid(const nlohmann::json& metadata, double price, const nlohmann::json& overrides) {
    if (!std::isfinite(price) || price <= 0) throw std::runtime_error("grid needs a positive reference price");
    const auto quoteText = metadata.at("quote_increment").get<std::string>();
    const auto quote = decimal(quoteText);
    const auto base = decimal(metadata.at("base_increment").get<std::string>());
    const auto target = price * 0.0001;
    const auto decade = std::pow(10.0, std::floor(std::log10(target)));
    double tick = decade;
    for (double m : {2.,5.,10.}) if (std::abs(m * decade - target) <= std::abs(tick - target)) tick = m * decade;
    // Keep a native-increment multiple, including non power-of-ten increments.
    tick = std::ceil(std::max(tick, quote) / quote - 1e-9) * quote;
    Grid grid{scaleFor(quoteText),tick,next125(next125(tick)),base};
    if (metadata.at("product_id") == "BTC-USD") { grid.nearTick = 1; grid.deepTick = 5; }
    grid.priceScale = overrides.value("price_scale", grid.priceScale);
    grid.nearTick = overrides.value("near_tick", grid.nearTick);
    grid.deepTick = overrides.value("deep_tick", grid.deepTick);
    grid.sizeFloor = overrides.value("size_floor", grid.sizeFloor);
    if (!std::isfinite(grid.priceScale) || grid.priceScale < scaleFor(quoteText) ||
        !std::isfinite(grid.sizeFloor) || grid.sizeFloor <= 0 || price * grid.priceScale >= 0x1p53)
        throw std::runtime_error("unrepresentable product grid");
    for (auto* t : {&grid.nearTick, &grid.deepTick}) {
        if (!std::isfinite(*t) || *t <= 0) throw std::runtime_error("invalid tick override");
        *t = std::ceil(std::max(*t, quote) / quote - 1e-9) * quote;
        const auto units = *t * grid.priceScale;
        if (units < 1 || units >= 0x1p53 || std::abs(units - std::round(units)) > 1e-6)
            throw std::runtime_error("tick is not representable in price units");
    }
    return grid;
}
recording::RecorderConfig Grid::config(const std::filesystem::path& root) const {
    recording::RecorderConfig c;
    c.root = root; c.priceScale = priceScale; c.sizeScale.floor = sizeFloor;
    c.layers = {{"near",std::llround(nearTick*priceScale),.95,1.05,false},
                {"deep",std::llround(deepTick*priceScale),.25,4.,true}};
    c.blockingQueue = c.deterministicResume = true;
    return c;
}
} // namespace sentinel::roller
