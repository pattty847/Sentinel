#include "AlgoOrderSpans.hpp"

#include <unordered_set>

void AlgoOrderSpans::apply(const trading::AlgoOrderEvent& ev) {
    if (ev.orderId.empty()) {
        m_markers.push_back(ev);
    } else {
        auto& span = m_spans[ev.orderId];
        span.symbol = ev.symbol;
        if (ev.status == trading::OrderStatus::Open) {
            span.side = ev.side;
            span.price = ev.price;
            span.openedMs = ev.timestampMs;
            span.closedMs = 0;
            span.terminalStatus = trading::OrderStatus::New;
            span.active = true;
        } else if (ev.status == trading::OrderStatus::Filled ||
                   ev.status == trading::OrderStatus::Canceled ||
                   ev.status == trading::OrderStatus::Rejected) {
            if (!span.active && span.openedMs == 0) {
                span.side = ev.side;
                span.price = ev.price;
                span.openedMs = ev.timestampMs;
            }
            span.closedMs = ev.timestampMs;
            span.terminalStatus = ev.status;
            span.active = false;
            m_markers.push_back(ev);
        }
    }
    while (m_markers.size() > kMaxMarkers) m_markers.pop_front();
}

void AlgoOrderSpans::reconcile(const trading::TradingSnapshot& snapshot, int64_t atMs) {
    if (!m_epoch.empty() && snapshot.epoch != m_epoch) {
        std::unordered_map<std::string, Span> retired;
        const std::string prefix = "retired-" + std::to_string(++m_retiredEpochs) + "/";
        for (auto& [id, span] : m_spans) {
            if (span.active) {
                span.active = false;
                span.closedMs = atMs;
                span.terminalStatus = trading::OrderStatus::Canceled;
            }
            retired.emplace(prefix + id, span);
        }
        m_spans = std::move(retired);
    }
    m_epoch = snapshot.epoch;

    std::unordered_set<std::string> held;
    for (const auto& order : snapshot.openOrders) {
        if (order.algoId.empty()) continue;
        held.insert(order.orderId);
        auto& span = m_spans[order.orderId];
        if (span.active) continue;
        span.symbol = order.symbol;
        span.side = order.side;
        span.price = order.limitPrice > 0.0 ? order.limitPrice : order.avgPrice;
        span.openedMs = atMs;
        span.closedMs = 0;
        span.terminalStatus = trading::OrderStatus::New;
        span.active = true;
    }
    for (auto& [id, span] : m_spans) {
        if (!span.active || held.contains(id)) continue;
        span.active = false;
        span.closedMs = atMs;
        span.terminalStatus = trading::OrderStatus::Canceled;
    }
}

size_t AlgoOrderSpans::activeCount() const {
    size_t n = 0;
    for (const auto& [_, span] : m_spans) n += span.active ? 1 : 0;
    return n;
}
