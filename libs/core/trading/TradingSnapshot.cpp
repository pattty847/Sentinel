#include "TradingSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace trading {

const PositionUpdate* TradingSnapshot::positionFor(const std::string& symbol) const {
    for (const auto& p : positions) {
        if (p.symbol == symbol) return &p;
    }
    return nullptr;
}

const RunningAlgo* TradingSnapshot::runningAlgo(const std::string& algoId) const {
    for (const auto& a : runningAlgos) {
        if (a.algoId == algoId) return &a;
    }
    return nullptr;
}

bool isActiveOrderStatus(OrderStatus status) {
    return status == OrderStatus::New || status == OrderStatus::Open || status == OrderStatus::Partial;
}

bool TradingAccountState::replace(const TradingSnapshot& snapshot) {
    const bool newEpoch = snapshot.epoch != m_state.epoch;
    m_state = snapshot;
    m_known = true;
    return newEpoch;
}

void TradingAccountState::apply(const OrderUpdate& update) {
    auto& orders = m_state.openOrders;
    const auto it = std::find_if(orders.begin(), orders.end(),
                                 [&](const OrderUpdate& o) { return o.orderId == update.orderId; });
    if (!isActiveOrderStatus(update.status)) {
        if (it != orders.end()) orders.erase(it);
    } else if (it != orders.end()) {
        *it = update;
    } else {
        orders.push_back(update);
    }
}

void TradingAccountState::apply(const PositionUpdate& update) {
    auto& positions = m_state.positions;
    const auto it = std::find_if(positions.begin(), positions.end(),
                                 [&](const PositionUpdate& p) { return p.symbol == update.symbol; });
    if (it != positions.end()) *it = update;
    else positions.push_back(update);
}

void TradingAccountState::apply(const RiskOrderUpdate& update) {
    auto& risk = m_state.riskOrders;
    const auto it = std::find_if(risk.begin(), risk.end(),
                                 [&](const RiskOrderUpdate& r) { return r.symbol == update.symbol; });
    if (it != risk.end()) *it = update;
    else risk.push_back(update);
}

std::optional<RiskOrderUpdate> TradingAccountState::riskFor(const std::string& symbol) const {
    if (!m_known) return std::nullopt;
    for (const auto& r : m_state.riskOrders) {
        if (r.symbol == symbol) return r;
    }
    RiskOrderUpdate none;
    none.symbol = symbol;
    return none;
}

std::vector<OrderUpdate> TradingAccountState::openOrdersFor(const std::string& symbol) const {
    std::vector<OrderUpdate> out;
    if (!m_known) return out;
    for (const auto& o : m_state.openOrders) {
        if (o.symbol == symbol) out.push_back(o);
    }
    return out;
}

std::optional<PositionUpdate> TradingAccountState::positionFor(const std::string& symbol) const {
    if (!m_known) return std::nullopt;
    if (const auto* p = m_state.positionFor(symbol)) return *p;
    return PositionUpdate{symbol, 0.0, 0.0, 0.0, 0.0};
}

std::vector<std::string> TradingAccountState::symbolsWithPosition() const {
    std::vector<std::string> out;
    if (!m_known) return out;
    for (const auto& p : m_state.positions) {
        if (std::abs(p.positionQty) > 1e-12) out.push_back(p.symbol);
    }
    std::sort(out.begin(), out.end());
    return out;
}

nlohmann::json tradingSnapshotJson(const TradingSnapshot& s) {
    auto positions = nlohmann::json::array();
    for (const auto& p : s.positions) {
        positions.push_back({{"symbol", p.symbol},
                             {"position_qty", p.positionQty},
                             {"avg_price", p.avgPrice},
                             {"unrealized_pnl", p.unrealizedPnl},
                             {"realized_pnl", p.realizedPnl}});
    }
    auto orders = nlohmann::json::array();
    for (const auto& o : s.openOrders) {
        orders.push_back({{"order_id", o.orderId},
                          {"symbol", o.symbol},
                          {"status", toString(o.status)},
                          {"side", toString(o.side)},
                          {"qty", o.qty},
                          {"filled_qty", o.filledQty},
                          {"remaining_qty", o.remainingQty},
                          {"avg_price", o.avgPrice},
                          {"limit_price", o.limitPrice},
                          {"algo_id", o.algoId}});
    }
    auto risk = nlohmann::json::array();
    for (const auto& r : s.riskOrders) {
        risk.push_back({{"symbol", r.symbol},
                        {"has_take_profit", r.hasTakeProfit},
                        {"take_profit_price", r.takeProfitPrice},
                        {"has_stop_loss", r.hasStopLoss},
                        {"stop_loss_price", r.stopLossPrice}});
    }
    auto algos = nlohmann::json::array();
    for (const auto& a : s.runningAlgos) {
        algos.push_back({{"algo_id", a.algoId}, {"symbol", a.symbol}});
    }
    return {{"type", "trading_snapshot"},
            {"schema_version", TradingSnapshot::kSchemaVersion},
            {"epoch", s.epoch},
            {"reason", s.reason},
            {"command_id", s.commandId},
            {"positions", std::move(positions)},
            {"open_orders", std::move(orders)},
            {"risk_orders", std::move(risk)},
            {"running_algos", std::move(algos)}};
}

std::optional<TradingSnapshot> parseTradingSnapshotJson(const nlohmann::json& msg) {
    if (!msg.is_object() || msg.value("type", "") != "trading_snapshot") return std::nullopt;
    const auto version = msg.find("schema_version");
    if (version == msg.end() || !version->is_number_integer() ||
        version->get<int>() != TradingSnapshot::kSchemaVersion) {
        return std::nullopt;
    }
    for (const char* key : {"positions", "open_orders", "risk_orders", "running_algos"}) {
        const auto it = msg.find(key);
        if (it == msg.end() || !it->is_array()) return std::nullopt;
    }
    TradingSnapshot s;
    s.epoch = msg.value("epoch", "");
    s.reason = msg.value("reason", "");
    s.commandId = msg.value("command_id", "");
    for (const auto& j : msg["positions"]) {
        PositionUpdate p;
        p.symbol = j.value("symbol", "");
        p.positionQty = j.value("position_qty", 0.0);
        p.avgPrice = j.value("avg_price", 0.0);
        p.unrealizedPnl = j.value("unrealized_pnl", 0.0);
        p.realizedPnl = j.value("realized_pnl", 0.0);
        if (!p.symbol.empty()) s.positions.push_back(std::move(p));
    }
    for (const auto& j : msg["open_orders"]) {
        OrderUpdate o;
        o.orderId = j.value("order_id", "");
        o.symbol = j.value("symbol", "");
        o.status = orderStatusFromString(j.value("status", "REJECTED"));
        o.side = orderSideFromString(j.value("side", "UNKNOWN"));
        o.qty = j.value("qty", 0.0);
        o.filledQty = j.value("filled_qty", 0.0);
        o.remainingQty = j.value("remaining_qty", 0.0);
        o.avgPrice = j.value("avg_price", 0.0);
        o.limitPrice = j.value("limit_price", 0.0);
        o.algoId = j.value("algo_id", "");
        if (!o.orderId.empty() && isActiveOrderStatus(o.status)) s.openOrders.push_back(std::move(o));
    }
    for (const auto& j : msg["risk_orders"]) {
        RiskOrderUpdate r;
        r.symbol = j.value("symbol", "");
        r.hasTakeProfit = j.value("has_take_profit", false);
        r.takeProfitPrice = j.value("take_profit_price", 0.0);
        r.hasStopLoss = j.value("has_stop_loss", false);
        r.stopLossPrice = j.value("stop_loss_price", 0.0);
        if (!r.symbol.empty()) s.riskOrders.push_back(std::move(r));
    }
    for (const auto& j : msg["running_algos"]) {
        RunningAlgo a{j.value("algo_id", ""), j.value("symbol", "")};
        if (!a.algoId.empty()) s.runningAlgos.push_back(std::move(a));
    }
    return s;
}

} // namespace trading
