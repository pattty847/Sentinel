#include "DomModel.hpp"
#include <QColor>
#include <algorithm>
#include <cmath>
#include <limits>

void DomTradeWindow::ingest(const Trade& trade)
{
    // Keep every event in the window, including unknown/invalid-price executions.
    // Invalid prices cannot be placed on the ladder; quantities are never counts.
    m_entries[m_next] = {trade.price, trade.side};
    m_next = (m_next + 1) % Capacity;
    m_size = std::min(m_size + 1, Capacity);
}

QString DomFreshness::text(qint64 nowMs) const
{
    const QString age = lastChangeMs > 0
        ? QStringLiteral("last change %1 s").arg(std::max<qint64>(0, nowMs - lastChangeMs) / 1000.0, 0, 'f', 1)
        : QStringLiteral("no book received");
    if (connected == false) return QStringLiteral("Disconnected · %1").arg(age);
    if (snapshotStale) return QStringLiteral("Stale snapshot · %1").arg(age);
    if (awaitingBook || lastChangeMs <= 0) return QStringLiteral("Waiting for book · %1").arg(age);
    if (connected == true) return QStringLiteral("Connected · %1").arg(age);
    return QStringLiteral("Connection unknown · %1").arg(age);
}

DomModel::DomModel(QObject* parent) : QAbstractTableModel(parent)
{
    m_bidBuffer.reserve(1);
    m_askBuffer.reserve(1);
}

std::optional<qint64> DomModel::bucket(double price, double tick)
{
    if (!std::isfinite(price) || !std::isfinite(tick) || price < 0 || tick <= 0) return {};
    const double key = price / tick;
    // Reserve headroom for window arithmetic and avoid llround overflow.
    if (!std::isfinite(key) || key > static_cast<double>(std::numeric_limits<qint64>::max() / 2)) return {};
    return std::llround(key);
}

QString DomModel::priceText(double price, double tick)
{
    int decimals = 0;
    while (decimals < 12 && (tick < 1 || std::abs(tick - std::round(tick)) > 1e-9)) {
        tick *= 10;
        ++decimals;
    }
    return QString::number(price, 'f', decimals);
}

int DomModel::rowCount(const QModelIndex& parent) const { return parent.isValid() || !m_ready ? 0 : Rows; }
int DomModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : Columns; }

QVariant DomModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= Columns) return {};
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};
    switch (section) {
    case Bid: return QStringLiteral("Bid (%1)").arg(m_base);
    case Price: return QStringLiteral("Price (%1)").arg(m_quote);
    case Ask: return QStringLiteral("Ask (%1)").arg(m_base);
    case Buys: return QStringLiteral("Buy count");
    case Sells: return QStringLiteral("Sell count");
    default: return QStringLiteral("Δ count");
    }
}

QVariant DomModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() >= Columns) return {};
    const auto& row = m_rows[index.row()];
    const int col = index.column();
    if (role == BucketRole) return m_top - index.row();
    if (role == BestSideRole) return row.bestBid && row.bestAsk ? "Bid / Ask" : row.bestBid ? "Bid" : row.bestAsk ? "Ask" : "";
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == QuantityRole) return col == Bid ? row.bid : col == Ask ? row.ask : 0.0;
    static const QColor bidColor("#56cbd4"), askColor("#e5b65b"), buyColor("#65ce8a"), sellColor("#ee8585"), bestColor("#29323b");
    if (role == Qt::ForegroundRole) {
        if (col == Bid) return bidColor;
        if (col == Ask) return askColor;
        if (col == Buys || (col == Delta && row.buys > row.sells)) return buyColor;
        if (col == Sells || (col == Delta && row.buys < row.sells)) return sellColor;
    }
    if (role == Qt::BackgroundRole && (row.bestBid || row.bestAsk)) return bestColor;
    if (role == Qt::ToolTipRole) {
        if (col >= Buys) return QStringLiteral("Execution counts in the last 1,000 trades; unknown aggressors excluded from buy, sell and delta.");
        return col == Price ? QStringLiteral("%1 · aggregated price bucket, not an exchange spread").arg(data(index, BestSideRole).toString())
                            : QStringLiteral("Resting %1 quantity in %2").arg(col == Bid ? "bid" : "ask", m_base);
    }
    if (role != Qt::DisplayRole) return {};
    switch (col) {
    case Bid: return row.bid > 0 ? QString::number(row.bid, 'g', 8) : QString();
    case Ask: return row.ask > 0 ? QString::number(row.ask, 'g', 8) : QString();
    case Price: return priceText((m_top - index.row()) * m_tick, m_tick);
    case Buys: return row.buys;
    case Sells: return row.sells;
    default: return row.buys - row.sells;
    }
}

void DomModel::clear(const QString& symbol)
{
    beginResetModel();
    const auto parts = symbol.split('-');
    m_base = parts.size() == 2 ? parts[0] : QStringLiteral("base");
    m_quote = parts.size() == 2 ? parts[1] : QStringLiteral("quote");
    m_ready = false;
    m_aggregationIssue.clear();
    m_tick = m_bestBid = m_bestAsk = 0;
    m_trades = m_unknown = 0;
    m_rows.fill({});
    endResetModel();
    emit headerDataChanged(Qt::Horizontal, 0, Columns - 1);
}

void DomModel::publish(const LiveOrderBook& book, const DomTradeWindow& trades, bool follow)
{
    m_trades = trades.size();
    m_unknown = 0;
    for (int i = 0; i < trades.size(); ++i) {
        const auto side = trades.entries()[i].side;
        if (side != AggressorSide::Buy && side != AggressorSide::Sell) ++m_unknown;
    }
    const auto view = book.captureDenseNonZero(m_bidBuffer, m_askBuffer, 1);
    const double tick = view.tickSize;
    const auto origin = bucket(view.minPrice, tick);
    m_aggregationIssue.clear();
    if (!origin) {
        m_aggregationIssue = QStringLiteral("Aggregation unavailable for this product");
        return;
    }
    if ((!view.bidLevels.empty() && view.minPrice + view.bidLevels.front().first * tick <= 0) ||
        (!view.askLevels.empty() && view.minPrice + view.askLevels.front().first * tick <= 0)) {
        if (m_ready) {
            beginRemoveRows({}, 0, Rows - 1);
            m_ready = false;
            endRemoveRows();
        }
        m_bestBid = m_bestAsk = 0;
        m_tick = tick;
        m_aggregationIssue = QStringLiteral("Aggregation too coarse for this product");
        return;
    }
    m_bestBid = view.bidLevels.empty() ? 0 : view.minPrice + view.bidLevels.front().first * tick;
    m_bestAsk = view.askLevels.empty() ? 0 : view.minPrice + view.askLevels.front().first * tick;
    const double mid = m_bestBid > 0 && m_bestAsk > 0 ? (m_bestBid + m_bestAsk) / 2 : m_bestBid + m_bestAsk;
    const auto center = bucket(mid, tick);
    if ((!center || mid <= 0) && !m_ready) {
        m_tick = tick;
        return;
    }
    if (center && mid > 0) m_center = *center;
    const bool first = !m_ready;
    const bool newTick = tick != m_tick;
    const qint64 top = first || follow ? std::max<qint64>(Rows - 1, m_center + Rows / 2)
        : newTick ? std::max<qint64>(Rows - 1, std::llround(m_top * m_tick / tick)) : m_top;
    const bool moved = top != m_top || newTick;
    if (first) beginInsertRows({}, 0, Rows - 1);
    m_top = top;
    m_tick = tick;
    m_ready = true;
    std::array<Row, Rows> next{};
    // The datasource's replica and this publisher both belong to the GUI thread.
    // Index by integer bucket, never floating subtraction/floor at row boundaries.
    const auto& bids = book.getBids();
    const auto& asks = book.getAsks();
    double maxBid = 0, maxAsk = 0;
    const auto bestBid = bucket(m_bestBid, tick), bestAsk = bucket(m_bestAsk, tick);
    for (int r = 0; r < Rows; ++r) {
        const qint64 key = m_top - r;
        const qint64 source = key - *origin;
        auto& row = next[r];
        if (source >= 0 && source < static_cast<qint64>(bids.size())) {
            if (std::isfinite(bids[source]) && bids[source] > 0) row.bid = bids[source];
            if (std::isfinite(asks[source]) && asks[source] > 0) row.ask = asks[source];
        }
        row.bestBid = m_bestBid > 0 && bestBid == key;
        row.bestAsk = m_bestAsk > 0 && bestAsk == key;
        maxBid = std::max(maxBid, row.bid);
        maxAsk = std::max(maxAsk, row.ask);
    }
    for (int i = 0; i < trades.size(); ++i) {
        const auto& trade = trades.entries()[i];
        // Mirror LiveOrderBook::price_to_index exactly, including its origin and
        // floating truncation at decimal boundaries (no absolute-grid epsilon).
        if (!std::isfinite(trade.price) || trade.price < view.minPrice || trade.price > view.maxPrice) continue;
        const double offset = (trade.price - view.minPrice) / tick;
        if (!std::isfinite(offset) || offset >= double(std::numeric_limits<qint64>::max() / 2)) continue;
        const auto key = std::optional<qint64>(*origin + static_cast<qint64>(offset));
        if (!key || m_top - *key < 0 || m_top - *key >= Rows) continue;
        auto& row = next[m_top - *key];
        if (trade.side == AggressorSide::Buy) ++row.buys;
        else if (trade.side == AggressorSide::Sell) ++row.sells;
    }
    const bool scaleChanged = maxBid != m_maxBid || maxAsk != m_maxAsk;
    m_maxBid = maxBid;
    m_maxAsk = maxAsk;
    int firstChanged = Rows, lastChanged = -1;
    for (int r = 0; r < Rows; ++r) {
        if (moved || scaleChanged || !(next[r] == m_rows[r])) {
            firstChanged = std::min(firstChanged, r);
            lastChanged = r;
        }
    }
    m_rows = next;
    if (first) endInsertRows();
    else if (lastChanged >= firstChanged) emit dataChanged(index(firstChanged, 0), index(lastChanged, Columns - 1));
}

int DomModel::centerRow() const { return m_ready ? int(std::clamp<qint64>(m_top - m_center, 0, Rows - 1)) : -1; }
QString DomModel::aggregation() const
{
    if (!m_aggregationIssue.isEmpty()) return m_aggregationIssue;
    return QStringLiteral("Aggregation: %1 %2 · resting size: %3").arg(m_tick > 0 ? priceText(m_tick, m_tick) : "—", m_quote, m_base);
}
QString DomModel::executionSummary() const
{
    return QStringLiteral("Execution counts · last 1,000 trades (%1 received) · unknown: %2 (excluded from Δ)").arg(m_trades).arg(m_unknown);
}
QString DomModel::summary() const
{
    const QString bid = m_bestBid > 0 ? priceText(m_bestBid, m_tick) : "—";
    const QString ask = m_bestAsk > 0 ? priceText(m_bestAsk, m_tick) : "—";
    const QString spread = m_bestBid > 0 && m_bestAsk > 0 ? priceText(m_bestAsk - m_bestBid, m_tick) : "—";
    return QStringLiteral("Bid %1 · Ask %2 · Bucketed spread %3 %4").arg(bid, ask, spread, m_quote);
}
