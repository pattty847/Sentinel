#include "FlowScales.hpp"
#include "roller/Grid.hpp"
#include <cmath>
#include <stdexcept>

namespace sentinel::flow {
FlowScales flowScales(const roller::Grid& grid, const nlohmann::json& metadata) {
    auto text = metadata.at("base_increment").get<std::string>();
    const auto dot = text.find('.');
    if (text.empty() || text.find_first_not_of("0123456789.") != std::string::npos ||
        text.front() == '.' || text.back() == '.' || text.find('.', dot == std::string::npos ? 0 : dot + 1) != std::string::npos ||
        std::stod(text) <= 0)
        throw std::invalid_argument("invalid flow base increment");
    if (dot != std::string::npos)
        while (text.size() > dot + 1 && text.back() == '0') text.pop_back();
    const size_t places = dot == std::string::npos ? 0 : text.size() - dot - 1;
    if (places > 15) throw std::invalid_argument("base increment precision exceeds 15");
    int64_t base = 1;
    for (size_t i = 0; i < places; ++i) base *= 10;
    if (!std::isfinite(grid.priceScale) || grid.priceScale < 1 || grid.priceScale >= 0x1p53 ||
        std::round(grid.priceScale) != grid.priceScale || !std::isfinite(grid.nearTick) || grid.nearTick <= 0 ||
        !std::isfinite(grid.nearTick * grid.priceScale) ||
        grid.nearTick * grid.priceScale >= 0x1p53 ||
        std::abs(std::round(grid.nearTick * grid.priceScale) - grid.nearTick * grid.priceScale) > 1e-6)
        throw std::invalid_argument("invalid flow recorder grid");
    return {grid.nearTick, std::llround(grid.nearTick * grid.priceScale),
            std::llround(grid.priceScale), base, 100'000'000};
}
} // namespace sentinel::flow
