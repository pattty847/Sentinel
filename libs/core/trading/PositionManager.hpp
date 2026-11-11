#pragma once

#include "TradingTypes.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

class PositionManager {
public:
    Position applyFill(const std::string& symbol, OrderSide side, double qty, double fillPrice, double markPrice);
    Position markToMarket(const std::string& symbol, double markPrice) const;
    std::optional<Position> get(const std::string& symbol) const;
    std::vector<Position> all() const;

private:
    static double signForSide(OrderSide side);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Position> m_positions;
};

} // namespace trading
