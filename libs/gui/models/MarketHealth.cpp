#include "MarketHealth.hpp"
#include <QDateTime>
#include <algorithm>

QString MarketHealth::stateText(State state) {
    switch (state) {
    case State::Live: return "Live";
    case State::Initializing: return "Initializing";
    case State::WaitingForSubscription: return "Waiting for subscription";
    case State::WaitingForBook: return "Waiting for book";
    case State::WaitingForHistory: return "Waiting for heatmap/history";
    case State::Reconnecting: return "Reconnecting";
    case State::Stale: return "Stale";
    case State::HistoryPartial: return "History partial";
    case State::Unavailable: return "Unavailable";
    case State::Disconnected: return "Disconnected";
    }
    return "Unavailable";
}
QString MarketHealth::ageText(std::optional<qint64> ageMs) {
    if (!ageMs) return "Unknown";
    const double seconds = std::max<qint64>(0, *ageMs) / 1000.0;
    if (seconds < 60) return QString::number(seconds, 'f', 1) + "s";
    if (seconds < 3600) return QString::number(seconds / 60, 'f', 0) + "m";
    return QString::number(seconds / 3600, 'g', 2) + "h";
}
QString MarketHealth::Snapshot::text() const { return MarketHealth::stateText(state); }
QString MarketHealth::Snapshot::compact() const {
    return text() + (bookAgeMs ? QStringLiteral(" · book ") + MarketHealth::ageText(bookAgeMs) : QString{});
}
void MarketHealth::setActiveSymbol(const QString& symbol) {
    if (m_activeSymbol == symbol) return;
    m_activeSymbol = symbol;
    m_chart = {};
    emit changed();
}
void MarketHealth::setTransport(Transport transport) {
    if (transport == m_transport) return;
    m_transport = transport;
    // Every transport epoch needs its own ack and accepted book. Retain receive
    // times only as explicitly aged last observations, never as readiness proof.
    for (auto& f : m_symbols) {
        f.acknowledged = false;
        f.bookReady = false;
        f.stale = false;
        f.refusal.clear();
        f.bookError.clear();
    }
    m_chart = {};
    emit changed();
}
void MarketHealth::subscriptionRequested(const QString& symbol) {
    m_symbols[symbol] = Facts{};
    m_symbols[symbol].requested = true;
    emit changed();
}
void MarketHealth::subscriptionAcknowledged(const QString& symbol) {
    auto it = m_symbols.find(symbol);
    if (it == m_symbols.end() || !it->requested || it->acknowledged) return;
    it->acknowledged = true;
    it->refusal.clear();
    emit changed();
}
void MarketHealth::subscriptionRefused(const QString& symbol, const QString& reason) {
    auto& f = m_symbols[symbol];
    f.refusal = reason.isEmpty() ? QStringLiteral("Subscription refused") : reason;
    f.acknowledged = false;
    emit changed();
}
void MarketHealth::subscriptionReleased(const QString& symbol) {
    m_symbols.remove(symbol);
    if (m_chart.symbol == symbol) m_chart = {};
    emit changed();
}
void MarketHealth::bookReceived(const QString& symbol, qint64 nowMs, bool snapshot) {
    auto it = m_symbols.find(symbol);
    if (it == m_symbols.end() || !it->requested || m_transport != Transport::Connected) return;
    if (!snapshot && !it->bookReady) return; // Only a snapshot establishes readiness in a new epoch.
    const bool transition = !it->bookReady || it->stale || !it->bookError.isEmpty();
    it->bookReady = true;
    it->stale = false;
    it->bookError.clear();
    it->bookMs = nowMs;
    if (transition) emit changed();
}
void MarketHealth::bookUnavailable(const QString& symbol, const QString& reason) {
    auto& f = m_symbols[symbol];
    f.bookReady = false;
    if (reason == "metadata_unavailable") f.bookError = "Book metadata unavailable; awaiting server retry";
    else if (reason == "aggregation_unavailable") f.bookError = "Book aggregation unavailable; awaiting snapshot";
    else if (reason == "invalidated") f.bookError = "Book invalidated; awaiting snapshot";
    else if (reason == "invalid_snapshot") f.bookError = "Invalid book snapshot";
    else if (reason == "invalid_tick") f.bookError = "Invalid book tick";
    else { f.bookError = (reason.isEmpty() || reason == "ready") ? QStringLiteral("Book snapshot unavailable") : reason; }
    emit changed();
}
void MarketHealth::bookStale(const QString& symbol) {
    auto& f = m_symbols[symbol];
    f.stale = true;
    emit changed();
}
void MarketHealth::heatmapReceived(const QString& symbol, qint64 nowMs) {
    auto it = m_symbols.find(symbol);
    if (it == m_symbols.end() || !it->requested || m_transport != Transport::Connected) return;
    const bool first = !it->heatmapMs;
    it->heatmapMs = nowMs;
    if (first) emit changed();
}
void MarketHealth::setChartState(const ChartFacts& facts) {
    if (facts == m_chart) return;
    m_chart = facts;
    emit changed();
}
void MarketHealth::setChartProvider(std::function<ChartFacts()> provider) {
    m_chartProvider = std::move(provider);
    setChartState({});
}
void MarketHealth::refreshChartState() {
    if (!m_chartProvider || m_refreshing) return;
    m_refreshing = true;
    setChartState(m_chartProvider());
    m_refreshing = false;
}
MarketHealth::Snapshot MarketHealth::snapshot(const QString& symbol) const {
    return snapshot(symbol.isEmpty() ? m_activeSymbol : symbol, QDateTime::currentMSecsSinceEpoch());
}
MarketHealth::Snapshot MarketHealth::snapshot(const QString& symbol, qint64 nowMs) const {
    Snapshot out;
    out.symbol = symbol;
    out.transport = m_transport;
    const auto f = m_symbols.value(symbol);
    out.acknowledged = f.acknowledged;
    if (f.bookMs) out.bookAgeMs = std::max<qint64>(0, nowMs - *f.bookMs);
    if (f.heatmapMs) out.heatmapAgeMs = std::max<qint64>(0, nowMs - *f.heatmapMs);
    const bool chart = !symbol.isEmpty() && m_chart.symbol == symbol;
    if (chart) {
        out.loading = m_chart.loading;
        out.partial = m_chart.partial;
        out.holding = m_chart.holding;
        out.coverage = m_chart.coverage;
    }
    if (m_transport == Transport::Disconnected) out.state = State::Disconnected;
    else if (m_transport == Transport::Reconnecting) out.state = State::Reconnecting;
    else if (m_transport == Transport::Initializing || symbol.isEmpty()) out.state = State::Initializing;
    else if (!f.refusal.isEmpty()) { out.state = State::Unavailable; out.reason = f.refusal; }
    else if (!f.acknowledged) out.state = State::WaitingForSubscription;
    else if (f.stale) { out.state = State::Stale; out.reason = "Book snapshot recovery timed out"; }
    else if (!f.bookError.isEmpty()) { out.state = State::Unavailable; out.reason = f.bookError; }
    else if (!f.bookReady) out.state = State::WaitingForBook;
    else if (chart && m_chart.unavailable == true) { out.state = State::Unavailable; out.reason = m_chart.reason; }
    else if (chart && (m_chart.loading == true || m_chart.holding == true)) {
        out.state = State::WaitingForHistory;
        out.reason = m_chart.reason.isEmpty() ? (m_chart.holding == true ? "Holding previous chart picture" : "Loading chart history") : m_chart.reason;
    } else if (chart && m_chart.partial == true) { out.state = State::HistoryPartial; out.reason = m_chart.reason; }
    else out.state = State::Live;
    return out;
}
