#include "DomModel.hpp"
#include <QColor>
#include <QDateTime>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <limits>

void DomTradeWindow::ingest(const Trade& trade)
{
    // Keep every event in the ring, including unknown/invalid-price executions.
    // Invalid prices cannot be placed on the ladder; a missing exchange time keeps 0.
    const qint64 timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count();
    if (m_size == Capacity) m_evictedMaxMs = std::max(m_evictedMaxMs, m_entries[m_next].timeMs);
    m_entries[m_next] = {trade.price, trade.size, std::max<qint64>(0, timeMs), trade.side};
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

QString DomModel::sizeText(double value)
{
    if (!std::isfinite(value)) return {};
    // Dust (a 1e-07 BTC trade) would print in e-notation; the tooltip keeps the value.
    if (value > 0 && value < 1e-4) return QStringLiteral("<.0001");
    static constexpr const char* suffixes[] = {"", "k", "m", "b"};
    double scaled = std::abs(value);
    int suffix = 0;
    // 999.5 rounds to 1000 at three significant digits: move to the next suffix first.
    while (suffix < 3 && scaled >= 999.5) {
        scaled /= 1000;
        ++suffix;
    }
    QString text = QString::number(scaled, 'g', 3);
    if (text.startsWith(QLatin1String("0."))) text.remove(0, 1);
    text += QLatin1String(suffixes[suffix]);
    return value < 0 ? QLatin1Char('-') + text : text;
}

QString DomModel::signedSizeText(double value)
{
    if (!std::isfinite(value)) return {};
    if (std::abs(value) < 1e-4) return QStringLiteral("≈0");
    return value > 0 ? QLatin1Char('+') + sizeText(value) : sizeText(value);
}

QString DomModel::fullSizeText(double value)
{
    // Book sizes reach the GUI as float values widened to double (BookDelta::qty),
    // so digits past float precision are noise: 0.0032f reads 0.0031999999.
    return QString::number(value, 'g', 7);
}

int DomModel::rowCount(const QModelIndex& parent) const { return parent.isValid() || !m_ready ? 0 : Rows; }
int DomModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : Columns; }

QVariant DomModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= Columns) return {};
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) {
        if (section >= Buys) return executionSummary();
        if (section == Price) return QString("Price in %1; aggregation %2").arg(m_quote, priceText(m_tick, m_tick));
        return QString("Resting %1 size in %2%3").arg(section == Bid ? "bid" : "ask", m_base,
            m_cumulative ? QStringLiteral(", cumulative from the touch outward") : QString());
    }
    if (role != Qt::DisplayRole) return {};
    switch (section) {
    case Bid: return m_cumulative ? QStringLiteral("Σ Bid") : QStringLiteral("Bid");
    case Price: return QStringLiteral("Price");
    case Ask: return m_cumulative ? QStringLiteral("Σ Ask") : QStringLiteral("Ask");
    case Buys: return QStringLiteral("Buy vol");
    case Sells: return QStringLiteral("Sell vol");
    default: return QStringLiteral("Δ vol");
    }
}

QVariant DomModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() || index.column() >= Columns) return {};
    const auto& row = m_rows[index.row()];
    const int col = index.column();
    const double delta = row.buys - row.sells;
    if (role == BucketRole) return m_top - index.row();
    if (role == BestSideRole) return row.bestBid && row.bestAsk ? "Bid / Ask" : row.bestBid ? "Bid" : row.bestAsk ? "Ask" : "";
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == QuantityRole) {
        switch (col) {
        case Bid: return row.bid;
        case Ask: return row.ask;
        case Buys: return row.buys;
        case Sells: return row.sells;
        case Delta: return delta;
        default: return 0.0;
        }
    }
    if (role == CumulativeRole) return col == Bid ? row.cumBid : col == Ask ? row.cumAsk : 0.0;
    if (role == Qt::ForegroundRole) {
        static const QColor buyColor("#65ce8a"), sellColor("#ee8585");
        if (col == Buys || (col == Delta && delta > 0)) return buyColor;
        if (col == Sells || (col == Delta && delta < 0)) return sellColor;
        return {};
    }
    if (role == Qt::ToolTipRole) {
        const QString volumes = QStringLiteral("Bought %1 · Sold %2 · Δ %3 %4")
            .arg(fullSizeText(row.buys), fullSizeText(row.sells), fullSizeText(delta), m_base);
        if (col >= Buys) return QStringLiteral("%1\n%2").arg(volumes, executionSummary());
        if (col == Price) return QStringLiteral("%1 · aggregated price bucket, not an exchange spread\n%2\n%3")
            .arg(data(index, BestSideRole).toString(), volumes, executionSummary());
        const bool bid = col == Bid;
        return QStringLiteral("%1 %2 resting %3\n%4 %2 cumulative from the touch\n%5")
            .arg(fullSizeText(bid ? row.bid : row.ask), m_base, bid ? "bid" : "ask",
                 fullSizeText(bid ? row.cumBid : row.cumAsk), executionSummary());
    }
    if (role != Qt::DisplayRole) return {};
    return cachedText(index.row(), col);
}

const QString& DomModel::cachedText(int r, int col) const
{
    // Paint asks for every visible cell each repaint; format only after a change.
    QString& text = m_text[size_t(r) * Columns + col];
    if (m_textValid[r] & (1u << col)) return text;
    m_textValid[r] |= uint8_t(1u << col);
    const auto& row = m_rows[r];
    const double delta = row.buys - row.sells;
    switch (col) {
    case Bid: {
        const double value = m_cumulative ? row.cumBid : row.bid;
        text = value > 0 ? sizeText(value) : QString();
        break;
    }
    case Ask: {
        const double value = m_cumulative ? row.cumAsk : row.ask;
        text = value > 0 ? sizeText(value) : QString();
        break;
    }
    case Price: text = priceText((m_top - r) * m_tick, m_tick); break;
    case Buys: text = row.buys > 0 ? sizeText(row.buys) : QString(); break;
    case Sells: text = row.sells > 0 ? sizeText(row.sells) : QString(); break;
    default: text = row.buys <= 0 && row.sells <= 0 ? QString() : signedSizeText(delta); break;
    }
    return text;
}

void DomModel::clear(const QString& symbol)
{
    beginResetModel();
    m_symbol = symbol;
    const auto parts = symbol.split('-');
    m_base = parts.size() == 2 ? parts[0] : QStringLiteral("base");
    m_quote = parts.size() == 2 ? parts[1] : QStringLiteral("quote");
    m_ready = false;
    m_aggregationIssue.clear();
    m_tick = m_bestBid = m_bestAsk = 0;
    m_windowTrades = m_unknown = m_untimed = m_ahead = 0;
    m_coveredMs = DomTradeWindow::WindowMs;
    m_nextExpiryMs = m_lastScaleMs = 0;
    m_scales.fill({});
    m_rows.fill({});
    m_textValid.fill(0);
    m_depthKey.reset();
    // No rows: nothing to rescale until the view reports a range for new rows.
    m_firstVisible = m_lastVisible = -1;
    m_visibleChanged = false;
    endResetModel();
    emit headerDataChanged(Qt::Horizontal, 0, Columns - 1);
}

void DomModel::setVisibleRows(int first, int last)
{
    if (first < 0 || last < 0) first = last = -1;
    if (first == m_firstVisible && last == m_lastVisible) return;
    // The first range the view reports replaces the all-rows guess at once.
    if (m_firstVisible < 0) m_lastScaleMs = 0;
    m_firstVisible = first;
    m_lastVisible = last;
    m_visibleChanged = true;
}

void DomModel::setCumulative(bool on)
{
    if (on == m_cumulative) return;
    m_cumulative = on;
    for (auto& valid : m_textValid) valid &= uint8_t(~((1u << Bid) | (1u << Ask)));
    emit headerDataChanged(Qt::Horizontal, Bid, Ask);
    if (m_ready) emit dataChanged(index(0, Bid), index(Rows - 1, Ask));
}

double DomModel::size(int row, bool bid, bool cumulativeSeries) const
{
    if (!m_ready || row < 0 || row >= Rows) return 0;
    const auto& r = m_rows[row];
    return cumulativeSeries ? (bid ? r.cumBid : r.cumAsk) : (bid ? r.bid : r.ask);
}

double DomModel::barFraction(double value, bool bid, bool cumulativeSeries) const
{
    const double maximum = m_scales[scaleIndex(bid, cumulativeSeries)].shown;
    if (!(maximum > 0) || !(value > 0)) return 0;
    return std::sqrt(std::min(1.0, value / maximum));
}

bool DomModel::needsRepublish(qint64 nowMs) const
{
    if (m_visibleChanged) return true;
    if (m_nextExpiryMs > 0 && nowMs >= m_nextExpiryMs) return true;
    return std::any_of(m_scales.begin(), m_scales.end(), [](const Scale& s) { return s.shown != s.target; });
}

uint8_t DomModel::textChanges(const Row& before, const Row& after, bool moved) const
{
    // Display strings by dependency: price on grid and window only; Bid/Ask on
    // the value the current mode shows; executions on their own volumes.
    if (moved) return 0xff;
    uint8_t bits = 0;
    if (m_cumulative ? before.cumBid != after.cumBid : before.bid != after.bid) bits |= 1u << Bid;
    if (m_cumulative ? before.cumAsk != after.cumAsk : before.ask != after.ask) bits |= 1u << Ask;
    if (before.buys != after.buys || before.sells != after.sells)
        bits |= (1u << Buys) | (1u << Sells) | (1u << Delta);
    return bits;
}

void DomModel::withdrawScaleWork()
{
    // Without a ladder there is nothing to scale: settle every request so the
    // dock does not republish each tick until rows return.
    m_scales.fill({});
    m_lastScaleMs = 0;
    m_visibleChanged = false;
    m_depthKey.reset();
}

bool DomModel::updateScales(qint64 nowMs)
{
    // A larger visible maximum applies at once (no bar overflows its cell); a
    // smaller one is approached exponentially so bars do not jump when a wall
    // scrolls out of view or is pulled.
    // No previous publication on this range (first, after clear, first visible report): snap.
    const bool snap = m_lastScaleMs <= 0;
    const double dt = snap ? 0 : std::clamp<double>(double(nowMs - m_lastScaleMs), 0, 1000);
    m_lastScaleMs = nowMs;
    const double keep = std::exp(-dt / ScaleTauMs);
    bool changed = false;
    for (auto& scale : m_scales) {
        const double before = scale.shown;
        if (snap || scale.target >= scale.shown || scale.target <= 0) scale.shown = scale.target;
        else {
            // Approach in log space so a 25x and a 2x drop both settle within ~0.5 s.
            const double ratio = std::log(scale.shown / scale.target) * keep;
            scale.shown = ratio <= 0.02 ? scale.target : scale.target * std::exp(ratio);
        }
        changed = changed || scale.shown != before;
    }
    return changed;
}

void DomModel::accumulateDepth(const LiveOrderBook& book, qint64 origin)
{
    // Cumulative size from the touch outward. Levels between the touch and a
    // pinned window edge count too, so the sums never depend on the scroll.
    // That walk only reads replica entries (never keys outside the band) and is
    // reused while book version, grid and window are unchanged.
    const qint64 bottom = m_top - (Rows - 1);
    const auto bestBid = bucket(m_bestBid, m_tick), bestAsk = bucket(m_bestAsk, m_tick);
    const DepthKey key{book.stateVersion(), origin, m_top, bestBid.value_or(0), bestAsk.value_or(0), m_tick};
    m_depthSteps = 0;
    if (!m_depthKey || !(*m_depthKey == key)) {
        const auto sum = [&](const std::vector<double>& side, qint64 fromKey, qint64 toKey) {
            const qint64 from = std::max<qint64>(fromKey - origin, 0);
            const qint64 to = std::min<qint64>(toKey - origin, static_cast<qint64>(side.size()) - 1);
            double total = 0;
            for (qint64 source = from; source <= to; ++source) {
                if (std::isfinite(side[source]) && side[source] > 0) total += side[source];
            }
            m_depthSteps += std::max<qint64>(0, to - from + 1);
            return total;
        };
        m_outsideBid = m_bestBid > 0 && bestBid && *bestBid > m_top ? sum(book.getBids(), m_top + 1, *bestBid) : 0;
        m_outsideAsk = m_bestAsk > 0 && bestAsk && *bestAsk < bottom ? sum(book.getAsks(), *bestAsk, bottom - 1) : 0;
        m_depthKey = key;
    }
    double bidTotal = m_outsideBid, askTotal = m_outsideAsk;
    for (int r = 0; r < Rows; ++r) {
        bidTotal += m_next[r].bid;
        m_next[r].cumBid = bidTotal;
        askTotal += m_next[Rows - 1 - r].ask;
        m_next[Rows - 1 - r].cumAsk = askTotal;
    }
}

void DomModel::addTrades(const DomTradeWindow& trades, const LiveOrderBook::DenseBookSnapshotView& view,
                         qint64 origin, qint64 nowMs)
{
    const double tick = view.tickSize;
    for (int i = 0; i < trades.size(); ++i) {
        const auto& trade = trades.entries()[i];
        if (!DomTradeWindow::inWindow(trade.timeMs, nowMs) || !std::isfinite(trade.size) || trade.size <= 0) continue;
        if (trade.side != AggressorSide::Buy && trade.side != AggressorSide::Sell) continue;
        // Place executions in the same containing bucket as the book replica.
        if (!std::isfinite(trade.price) || trade.price < view.minPrice || trade.price > view.maxPrice) continue;
        const size_t offset = LiveOrderBook::bucketIndex(trade.price, view.minPrice, tick);
        if (offset >= static_cast<size_t>(std::numeric_limits<qint64>::max() / 2)) continue;
        const qint64 key = origin + static_cast<qint64>(offset);
        if (m_top - key < 0 || m_top - key >= Rows) continue;
        auto& row = m_next[m_top - key];
        if (trade.side == AggressorSide::Buy) row.buys += trade.size;
        else row.sells += trade.size;
    }
}

void DomModel::publish(const LiveOrderBook& book, const DomTradeWindow& trades, bool follow, qint64 nowMs)
{
    if (nowMs < 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 cutoff = nowMs - DomTradeWindow::WindowMs;
    m_windowTrades = m_unknown = m_untimed = m_ahead = 0;
    m_nextExpiryMs = 0;
    const auto wakeAt = [&](qint64 ms) { m_nextExpiryMs = m_nextExpiryMs ? std::min(m_nextExpiryMs, ms) : ms; };
    for (int i = 0; i < trades.size(); ++i) {
        const auto& trade = trades.entries()[i];
        if (trade.timeMs <= 0) { ++m_untimed; continue; }
        if (trade.timeMs > nowMs + DomTradeWindow::SkewMs) {
            ++m_ahead; // stamped ahead of this clock: enters when its time comes
            wakeAt(trade.timeMs - DomTradeWindow::SkewMs);
            continue;
        }
        if (!DomTradeWindow::inWindow(trade.timeMs, nowMs)) continue;
        ++m_windowTrades;
        if (trade.side != AggressorSide::Buy && trade.side != AggressorSide::Sell) ++m_unknown;
        wakeAt(trade.timeMs + DomTradeWindow::WindowMs);
    }
    // Evicted trades inside the window mean volume is missing up to the newest of them.
    m_coveredMs = trades.evictedMaxMs() > cutoff
        ? std::clamp<qint64>(nowMs - trades.evictedMaxMs(), 0, DomTradeWindow::WindowMs) : DomTradeWindow::WindowMs;
    // The warning ends when the newest evicted trade leaves the minute: publish then.
    if (trades.evictedMaxMs() > cutoff) wakeAt(trades.evictedMaxMs() + DomTradeWindow::WindowMs);
    const auto view = book.captureDenseNonZero(m_bidBuffer, m_askBuffer, 1);
    const double tick = view.tickSize;
    const auto origin = bucket(view.minPrice, tick);
    m_aggregationIssue.clear();
    m_bestBid = view.bidLevels.empty() ? 0 : view.minPrice + view.bidLevels.front().first * tick;
    m_bestAsk = view.askLevels.empty() ? 0 : view.minPrice + view.askLevels.front().first * tick;
    const double mid = m_bestBid > 0 && m_bestAsk > 0 ? (m_bestBid + m_bestAsk) / 2 : m_bestBid + m_bestAsk;
    const bool coarse = (!view.bidLevels.empty() && m_bestBid <= 0) ||
        (!view.askLevels.empty() && m_bestAsk <= 0) || (mid > 0 && tick > mid * 0.01);
    if (!origin || coarse) {
        if (m_ready) {
            beginRemoveRows({}, 0, Rows - 1);
            m_ready = false;
            endRemoveRows();
        }
        m_bestBid = m_bestAsk = 0;
        m_tick = tick;
        withdrawScaleWork();
        m_aggregationIssue = !origin ? QStringLiteral("Aggregation unavailable for %1").arg(m_symbol)
            : QStringLiteral("Server aggregation %1 %2 is too coarse for %3").arg(priceText(tick, tick), m_quote, m_symbol);
        return;
    }
    const auto center = bucket(mid, tick);
    if ((!center || mid <= 0) && !m_ready) {
        m_tick = tick;
        withdrawScaleWork();
        return;
    }
    if (center && mid > 0) m_center = *center;
    const bool first = !m_ready;
    const bool newTick = tick != m_tick;
    const qint64 top = first || follow ? std::max<qint64>(Rows - 1, m_center + Rows / 2)
        : newTick ? std::max<qint64>(Rows - 1, std::llround(m_top * m_tick / tick)) : m_top;
    const bool moved = top != m_top || newTick;
    if (first) {
        beginInsertRows({}, 0, Rows - 1);
        m_textValid.fill(0);
    }
    m_top = top;
    m_tick = tick;
    m_ready = true;
    m_next.fill({});
    // The datasource's replica and this publisher both belong to the GUI thread.
    // Index by integer bucket, never floating subtraction/floor at row boundaries.
    const auto& bids = book.getBids();
    const auto& asks = book.getAsks();
    const auto bestBid = bucket(m_bestBid, tick), bestAsk = bucket(m_bestAsk, tick);
    for (int r = 0; r < Rows; ++r) {
        const qint64 key = m_top - r;
        const qint64 source = key - *origin;
        auto& row = m_next[r];
        if (source >= 0 && source < static_cast<qint64>(bids.size())) {
            if (std::isfinite(bids[source]) && bids[source] > 0) row.bid = bids[source];
            if (std::isfinite(asks[source]) && asks[source] > 0) row.ask = asks[source];
        }
        row.bestBid = m_bestBid > 0 && bestBid == key;
        row.bestAsk = m_bestAsk > 0 && bestAsk == key;
    }
    accumulateDepth(book, *origin);
    addTrades(trades, view, *origin, nowMs);
    // Scale targets: the maxima over the rows the view shows (all rows until it reports them).
    const int firstVisible = m_firstVisible < 0 ? 0 : std::clamp(m_firstVisible, 0, Rows - 1);
    const int lastVisible = m_lastVisible < 0 ? Rows - 1 : std::clamp(m_lastVisible, firstVisible, Rows - 1);
    std::array<double, 4> targets{};
    for (int r = firstVisible; r <= lastVisible; ++r) {
        const auto& row = m_next[r];
        targets[scaleIndex(true, false)] = std::max(targets[scaleIndex(true, false)], row.bid);
        targets[scaleIndex(false, false)] = std::max(targets[scaleIndex(false, false)], row.ask);
        targets[scaleIndex(true, true)] = std::max(targets[scaleIndex(true, true)], row.cumBid);
        targets[scaleIndex(false, true)] = std::max(targets[scaleIndex(false, true)], row.cumAsk);
    }
    for (size_t i = 0; i < m_scales.size(); ++i) m_scales[i].target = targets[i];
    m_visibleChanged = false;
    const bool scaleChanged = updateScales(nowMs);
    int firstChanged = Rows, lastChanged = -1;
    for (int r = 0; r < Rows; ++r) {
        const bool valueChanged = moved || !(m_next[r] == m_rows[r]);
        if (valueChanged) m_textValid[r] &= uint8_t(~textChanges(m_rows[r], m_next[r], moved));
        if (valueChanged || scaleChanged) {
            firstChanged = std::min(firstChanged, r);
            lastChanged = r;
        }
    }
    m_rows.swap(m_next);
    if (first) endInsertRows();
    else if (lastChanged >= firstChanged) emit dataChanged(index(firstChanged, 0), index(lastChanged, Columns - 1));
}

int DomModel::centerRow() const { return m_ready ? int(std::clamp<qint64>(m_top - m_center, 0, Rows - 1)) : -1; }
QString DomModel::aggregation(bool compact) const
{
    if (!m_aggregationIssue.isEmpty()) return compact
        ? (m_tick > 0 ? QString("Coarse: %1 %2").arg(priceText(m_tick, m_tick), m_quote) : QString("No aggregation"))
        : m_aggregationIssue;
    if (compact) return QString("Tick %1 %2 · %3 size").arg(m_tick > 0 ? priceText(m_tick, m_tick) : "—", m_quote, m_base);
    return QStringLiteral("Aggregation: %1 %2 · resting size: %3").arg(m_tick > 0 ? priceText(m_tick, m_tick) : "—", m_quote, m_base);
}
QString DomModel::executionSummary() const
{
    QString text = QStringLiteral("Traded volume in %1 · last %2 s by trade time · %3 trades · unknown side: %4 (excluded from buy/sell/Δ)")
        .arg(m_base).arg(DomTradeWindow::WindowMs / 1000).arg(m_windowTrades).arg(m_unknown);
    if (m_coveredMs < DomTradeWindow::WindowMs)
        text += QStringLiteral(" · ring-limited: the last %1 trades cover only %2 s")
            .arg(QLocale(QLocale::English).toString(DomTradeWindow::Capacity)).arg(m_coveredMs / 1000.0, 0, 'f', 1);
    if (m_untimed > 0) text += QStringLiteral(" · %1 without trade time not counted").arg(m_untimed);
    if (m_ahead > 0) text += QStringLiteral(" · %1 stamped more than %2 s ahead of this clock, not yet counted")
        .arg(m_ahead).arg(DomTradeWindow::SkewMs / 1000);
    return text;
}
QString DomModel::summary(bool compact) const
{
    if (!m_aggregationIssue.isEmpty()) return {};
    const QString bid = m_bestBid > 0 ? priceText(m_bestBid, m_tick) : "—";
    const QString ask = m_bestAsk > 0 ? priceText(m_bestAsk, m_tick) : "—";
    const QString spread = m_bestBid > 0 && m_bestAsk > 0 ? priceText(m_bestAsk - m_bestBid, m_tick) : "—";
    if (compact) return QString("Spr %1").arg(spread);
    return QStringLiteral("Bid %1 · Ask %2 · Bucketed spread %3 %4").arg(bid, ask, spread, m_quote);
}
