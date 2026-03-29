#pragma once

#include "TradingTypes.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading {

class OrderStore {
public:
    void upsert(const Order& order);
    std::optional<Order> get(const std::string& orderId) const;
    std::vector<Order> getAllActiveForSymbol(const std::string& symbol) const;
    std::vector<Order> getAllActive() const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Order> m_orders;
};

} // namespace trading
