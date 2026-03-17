#include "OrderStore.hpp"

namespace trading {

namespace {
bool isActive(const Order& o) {
    return o.status == OrderStatus::New || o.status == OrderStatus::Partial || o.status == OrderStatus::Open;
}
}

void OrderStore::upsert(const Order& order) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_orders[order.id] = order;
}

std::optional<Order> OrderStore::get(const std::string& orderId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_orders.find(orderId);
    if (it == m_orders.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Order> OrderStore::getAllActiveForSymbol(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Order> out;
    for (const auto& [_, order] : m_orders) {
        if (order.symbol == symbol && isActive(order)) {
            out.push_back(order);
        }
    }
    return out;
}

std::vector<Order> OrderStore::getAllActive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Order> out;
    for (const auto& [_, order] : m_orders) {
        if (isActive(order)) {
            out.push_back(order);
        }
    }
    return out;
}

} // namespace trading
