#include "DomModel.hpp"
#include "DomGrouping.hpp"
#include "SentinelLogging.hpp"
#include <QColor>
#include <QDateTime>
#include <QLocale>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
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
    // One-time: tags are refilled in place, so fresh flashes never allocate.
    for (auto& tag : m_pulseText) tag.reserve(PulseTagCapacity);
}

std::optional<qint64> DomModel::bucket(double price, double tick)
{
    if (!std::isfinite(price) || !std::isfinite(tick) || price < 0 || tick <= 0) return {};
    const double key = price / tick;
    // Keys stay below dom::MaxNativeKey: every group x key product of the window then fits
    // int64 and the Pulse classifier's price keys stay in range (DomGrouping.hpp).
    if (!std::isfinite(key) || key >= static_cast<double>(dom::MaxNativeKey)) return {};
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

int DomModel::formatSize(char* out, int capacity, double value)
{
    // The same text as sizeText, through locale-independent std::to_chars.
    if (!std::isfinite(value) || capacity <= 0) return 0;
    if (value > 0 && value < 1e-4) {
        static constexpr char dust[] = "<.0001";
        if (capacity < int(sizeof dust) - 1) return 0;
        std::memcpy(out, dust, sizeof dust - 1);
        return int(sizeof dust - 1);
    }
    static constexpr char suffixes[] = {0, 'k', 'm', 'b'};
    double scaled = std::abs(value);
    int suffix = 0;
    while (suffix < 3 && scaled >= 999.5) {
        scaled /= 1000;
        ++suffix;
    }
    char* const end = out + capacity;
    char* at = out;
    if (value < 0) *at++ = '-';
    const auto result = std::to_chars(at, end, scaled, std::chars_format::general, 3);
    if (result.ec != std::errc{}) return 0;
    if (result.ptr - at >= 2 && at[0] == '0' && at[1] == '.') {
        std::memmove(at, at + 1, size_t(result.ptr - at - 1));
        at = result.ptr - 1;
    } else at = result.ptr;
    if (suffix > 0) {
        if (at == end) return 0;
        *at++ = suffixes[suffix];
    }
    return int(at - out);
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
        if (section == Price) return QString("Price in %1: each row's lower edge; rows of %2, server tick %3")
            .arg(m_quote, priceText(m_tick, m_tick), priceText(m_nativeTick, m_nativeTick));
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
    m_tick = m_nativeTick = m_bestBid = m_bestAsk = 0;
    m_appliedRequest = m_requestedGroup;
    m_windowTrades = m_unknown = m_untimed = m_ahead = 0;
    m_coveredMs = DomTradeWindow::WindowMs;
    m_nextExpiryMs = m_lastScaleMs = 0;
    m_scales.fill({});
    m_rows.fill({});
    m_textValid.fill(0);
    m_depthKey.reset();
    ++m_pulseSeries;
    resetPulse();
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
    if (m_visibleChanged || m_requestedGroup != m_appliedRequest) return true;
    if (m_nextExpiryMs > 0 && nowMs >= m_nextExpiryMs) return true;
    // Liquidity Pulse: fading flashes repaint each tick, then their cleared rows once.
    if (m_pulseLive > 0 || m_repaintFirst <= m_repaintLast) return true;
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
    m_appliedRequest = m_requestedGroup; // applies with the next ladder
    m_depthKey.reset();
    clearFlashes(false);
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
    // Those sums read only replica entries (never keys outside the band) through
    // LiveOrderBook::sumRange (cached per 64-slot block, so a far-pinned window
    // costs its edges plus one read per block) and are reused while book version,
    // grid and window are unchanged.
    // Grouped keys of the filled rows' edges; the touch and the replica are native.
    const qint64 rangeTop = m_top - m_rangeFirst, rangeBottom = m_top - m_rangeLast;
    const int64_t g = m_group;
    const auto bestBid = bucket(m_bestBid, m_nativeTick), bestAsk = bucket(m_bestAsk, m_nativeTick);
    const DepthKey key{book.stateVersion(), origin, rangeTop, rangeBottom, bestBid.value_or(0), bestAsk.value_or(0),
                       m_nativeTick, g};
    m_depthSteps = 0;
    if (!m_depthKey || !(*m_depthKey == key)) {
        const auto sum = [&](bool bid, qint64 fromKey, qint64 toKey) {
            const qint64 from = std::max<qint64>(fromKey - origin, 0);
            const qint64 to = toKey - origin;
            if (to < from) return 0.0;
            size_t reads = 0;
            const double total = book.sumRange(bid, size_t(from), size_t(to), &reads);
            m_depthSteps += qint64(reads);
            return total;
        };
        m_outsideBid = m_bestBid > 0 && bestBid && *bestBid >= (rangeTop + 1) * g ? sum(true, (rangeTop + 1) * g, *bestBid) : 0;
        m_outsideAsk = m_bestAsk > 0 && bestAsk && *bestAsk < rangeBottom * g ? sum(false, *bestAsk, rangeBottom * g - 1) : 0;
        m_depthKey = key;
    }
    double bidTotal = m_outsideBid, askTotal = m_outsideAsk;
    for (int r = m_rangeFirst; r <= m_rangeLast; ++r) {
        bidTotal += m_next[r].bid;
        m_next[r].cumBid = bidTotal;
        askTotal += m_next[m_rangeLast + m_rangeFirst - r].ask;
        m_next[m_rangeLast + m_rangeFirst - r].cumAsk = askTotal;
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
        const qint64 rowKey = dom::floorDiv(key, m_group); // the grouped row holding the trade price
        if (m_top - rowKey < 0 || m_top - rowKey >= Rows) continue;
        auto& row = m_next[m_top - rowKey];
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
    // The whole band's keys must be below dom::MaxNativeKey (no ladder otherwise).
    auto origin = bucket(view.minPrice, tick);
    const qint64 bandLevels = qint64(std::min(book.getBids().size(), book.getAsks().size()));
    if (origin && *origin > dom::MaxNativeKey - bandLevels) origin.reset();
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
        m_tick = m_nativeTick = tick;
        withdrawScaleWork();
        m_aggregationIssue = !origin ? QStringLiteral("Aggregation unavailable for %1").arg(m_symbol)
            : QStringLiteral("Server aggregation %1 %2 is too coarse for %3").arg(priceText(tick, tick), m_quote, m_symbol);
        return;
    }
    const auto center = bucket(mid, tick);
    if ((!center || mid <= 0) && !m_ready) {
        m_tick = m_nativeTick = tick;
        withdrawScaleWork();
        return;
    }
    // A row as wide as the band already shows the whole book: never coarser (the bound
    // every product below relies on).
    const int64_t group = std::clamp<int64_t>(m_requestedGroup, 1, std::max<qint64>(1, bandLevels));
    m_appliedRequest = m_requestedGroup;
    if (center && mid > 0) m_center = dom::floorDiv(*center, group);
    const bool first = !m_ready;
    const bool newGrid = tick != m_nativeTick || group != m_group;
    // The window top never leaves the band by more than a window, whatever sequence of
    // regroups, pins and tick changes came before (no drift, products bounded).
    const qint64 topLimit = std::max<qint64>(Rows - 1, dom::floorDiv(*origin + bandLevels - 1, group) + Rows);
    const qint64 top = std::clamp<qint64>(first || follow ? m_center + Rows / 2
        : newGrid ? anchoredTop(tick, group) : m_top, Rows - 1, topLimit);
    const bool moved = top != m_top || newGrid;
    // Flashes stay on their price: shift with the window, drop on a new grid. A new
    // grid is a new Pulse series: the next step only observes (no events across grids).
    if (newGrid && !first) ++m_pulseSeries;
    if (first || newGrid) clearFlashes(false);
    else if (top != m_top) shiftFlashes(top - m_top);
    if (first) {
        beginInsertRows({}, 0, Rows - 1);
        m_textValid.fill(0);
    }
    m_top = top;
    m_nativeTick = tick;
    m_group = group;
    m_tick = double(group) * tick;
    m_ready = true;
    m_next.fill({});
    // The datasource's replica and this publisher both belong to the GUI thread.
    // Index by integer bucket, never floating subtraction/floor at row boundaries.
    groupRange(group);
    fillRows(book, *origin, group);
    const auto bestBid = bucket(m_bestBid, tick), bestAsk = bucket(m_bestAsk, tick);
    const auto markBest = [&](std::optional<qint64> native, bool bid) {
        if (!native || (bid ? m_bestBid : m_bestAsk) <= 0) return;
        const qint64 r = m_top - dom::floorDiv(*native, group);
        if (r >= 0 && r < Rows) (bid ? m_next[r].bestBid : m_next[r].bestAsk) = true;
    };
    markBest(bestBid, true);
    markBest(bestAsk, false);
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
        if (valueChanged || scaleChanged || pulseRepaint(r)) {
            firstChanged = std::min(firstChanged, r);
            lastChanged = r;
        }
    }
    m_repaintFirst = Rows;
    m_repaintLast = -1;
    m_rows.swap(m_next);
    if (first) endInsertRows();
    else if (lastChanged >= firstChanged) emit dataChanged(index(firstChanged, 0), index(lastChanged, Columns - 1));
}

void DomModel::setGroup(int64_t group)
{
    m_requestedGroup = std::clamp<int64_t>(group, 1, dom::MaxNativeKey);
}

qint64 DomModel::anchoredTop(double nativeTick, int64_t group) const
{
    // A new grid keeps the price at the middle of the rows on screen on its screen row.
    const int anchor = m_firstVisible >= 0
        ? (std::clamp(m_firstVisible, 0, Rows - 1) + std::clamp(m_lastVisible, 0, Rows - 1)) / 2 : Rows / 2;
    const qint64 old = m_top - anchor;
    // old <= the previous top limit, so old x m_group stays within the bound of DomGrouping.hpp;
    // the caller clamps the result to the new top limit.
    const double scaled = double(old) * m_tick / (double(group) * nativeTick);
    const qint64 key = nativeTick == m_nativeTick ? dom::floorDiv(old * m_group, group)
        : std::isfinite(scaled) && std::abs(scaled) < double(dom::MaxNativeKey) ? std::llround(scaled) : 0;
    return key + anchor;
}

void DomModel::groupRange(int64_t group)
{
    // Bounded by the rows on screen plus a margin, never the whole window, once rows sum levels.
    if (group <= 1) {
        m_rangeFirst = 0;
        m_rangeLast = Rows - 1;
        return;
    }
    const int center = int(std::clamp<qint64>(m_top - m_center, 0, Rows - 1));
    const int first = std::clamp(m_firstVisible >= 0 ? m_firstVisible : center - PulseMaxRows / 4, 0, Rows - 1);
    const int last = std::clamp(m_lastVisible >= 0 ? m_lastVisible : center + PulseMaxRows / 4, first, Rows - 1);
    const qint64 shown = last - first + 1;
    const qint64 span = std::min<qint64>(Rows, std::max<qint64>(shown + 2 * GroupMarginRows, GroupBudgetLevels / group));
    const int extra = int(span - shown);
    m_rangeFirst = std::max(0, first - extra / 2);
    m_rangeLast = std::min(Rows - 1, last + (extra - extra / 2));
}

void DomModel::fillRows(const LiveOrderBook& book, qint64 origin, int64_t group)
{
    // One pass over the native slots of the filled rows: slot s is level origin + s,
    // in row m_top - floorDiv(origin + s, group).
    const auto& bids = book.getBids();
    const auto& asks = book.getAsks();
    const qint64 size = static_cast<qint64>(std::min(bids.size(), asks.size()));
    const qint64 lo = std::max<qint64>((m_top - m_rangeLast) * group - origin, 0);
    const qint64 hi = std::min<qint64>((m_top - m_rangeFirst + 1) * group - origin, size);
    m_groupLevels = 0;
    if (group > qint64(2 * LiveOrderBook::SumBlockSlots)) {
        // Coarse rows (a linked $250 tick is 2,500 levels): per-row block sums, so a row
        // costs its two edges plus one read per block, not every level.
        for (int r = m_rangeFirst; r <= m_rangeLast; ++r) {
            const qint64 from = std::max<qint64>((m_top - r) * group - origin, 0);
            const qint64 to = std::min<qint64>((m_top - r + 1) * group - origin, size) - 1;
            if (to < from) continue;
            size_t bidReads = 0, askReads = 0;
            m_next[size_t(r)].bid = book.sumRange(true, size_t(from), size_t(to), &bidReads);
            m_next[size_t(r)].ask = book.sumRange(false, size_t(from), size_t(to), &askReads);
            m_groupLevels += qint64(std::max(bidReads, askReads));
        }
        return;
    }
    m_groupLevels = std::max<qint64>(0, hi - lo);
    for (qint64 source = lo; source < hi; ++source) {
        auto& row = m_next[size_t(m_top - dom::floorDiv(origin + source, group))];
        if (std::isfinite(bids[source]) && bids[source] > 0) row.bid += bids[source];
        if (std::isfinite(asks[source]) && asks[source] > 0) row.ask += asks[source];
    }
}

int DomModel::centerRow() const { return m_ready ? int(std::clamp<qint64>(m_top - m_center, 0, Rows - 1)) : -1; }
QString DomModel::aggregation(bool compact) const
{
    if (!m_aggregationIssue.isEmpty()) return compact
        ? (m_tick > 0 ? QString("Coarse: %1 %2").arg(priceText(m_tick, m_tick), m_quote) : QString("No aggregation"))
        : m_aggregationIssue;
    const QString native = m_nativeTick > 0 ? priceText(m_nativeTick, m_nativeTick) : QStringLiteral("—");
    if (m_group > 1 && m_nativeTick > 0) {
        const QString rows = priceText(m_tick, m_tick);
        if (compact) return QString("Rows %1 %2 · tick %3 · %4").arg(rows, m_quote, native, m_base);
        return QStringLiteral("Aggregation: rows of %1 %2 (%3 server ticks of %4) · resting size: %5")
            .arg(rows, m_quote).arg(m_group).arg(native, m_base);
    }
    if (compact) return QString("Tick %1 %2 · %3 size").arg(native, m_quote, m_base);
    return QStringLiteral("Aggregation: %1 %2 · resting size: %3").arg(native, m_quote, m_base);
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
    // The native top, whatever the row grouping.
    const QString bid = m_bestBid > 0 ? priceText(m_bestBid, m_nativeTick) : "—";
    const QString ask = m_bestAsk > 0 ? priceText(m_bestAsk, m_nativeTick) : "—";
    const QString spread = m_bestBid > 0 && m_bestAsk > 0 ? priceText(m_bestAsk - m_bestBid, m_nativeTick) : "—";
    if (compact) return QString("Spr %1").arg(spread);
    return QStringLiteral("Bid %1 · Ask %2 · Bucketed spread %3 %4").arg(bid, ask, spread, m_quote);
}

void DomModel::clearFlashes(bool repaint)
{
    if (repaint && m_pulseLive > 0) {
        for (int r = 0; r < Rows; ++r)
            if (m_flash[r][0].change || m_flash[r][0].eaten || m_flash[r][1].change || m_flash[r][1].eaten) markPulseRepaint(r);
    }
    if (!repaint) {
        m_repaintFirst = Rows;
        m_repaintLast = -1;
    }
    m_flash.fill({});
    m_pulseTextValid.fill(0);
    m_pulseLive = 0;
}

void DomModel::shiftFlashes(qint64 rows)
{
    // A window move by `rows` keys: old row r shows the price of new row r + rows.
    if (m_pulseLive == 0) return;
    if (rows >= Rows || rows <= -Rows) {
        clearFlashes(false);
        return;
    }
    const int d = int(rows);
    // Rotation, never assignment: every tag string keeps its reserved buffer.
    const auto rotateRows = [&](auto& values, int stride) {
        const size_t n = size_t(std::abs(d)) * stride;
        if (d > 0) std::rotate(values.begin(), values.end() - n, values.end());
        else std::rotate(values.begin(), values.begin() + n, values.end());
    };
    rotateRows(m_flash, 1);
    rotateRows(m_pulseText, 4);
    rotateRows(m_pulseTextValid, 1);
    // Rows that entered the window hold no flash and no valid tag.
    const int vacated = std::abs(d);
    for (int i = 0; i < vacated; ++i) {
        const int r = d > 0 ? i : Rows - 1 - i;
        m_flash[r] = {};
        m_pulseTextValid[r] = 0;
    }
    m_pulseLive = 0;
    for (const auto& row : m_flash)
        for (const auto& side : row) m_pulseLive += (side.change != 0) + (side.eaten != 0);
}

void DomModel::markPulseRepaint(int row)
{
    m_repaintFirst = std::min(m_repaintFirst, row);
    m_repaintLast = std::max(m_repaintLast, row);
}

bool DomModel::pulseRepaint(int row) const
{
    if (row >= m_repaintFirst && row <= m_repaintLast) return true;
    const auto& f = m_flash[row];
    return f[0].change || f[0].eaten || f[1].change || f[1].eaten;
}

void DomModel::expireFlashes(qint64 nowMs)
{
    if (m_pulseLive == 0) return;
    m_pulseLive = 0;
    for (int r = 0; r < Rows; ++r) {
        for (auto& f : m_flash[r]) {
            const int side = &f == &m_flash[r][0] ? 0 : 2;
            if (f.change && nowMs - f.changeAt >= PulseFadeMs) {
                f.change = 0;
                m_pulseTextValid[r] &= uint8_t(~(1u << side));
                markPulseRepaint(r);
            }
            if (f.eaten && nowMs - f.eatenAt >= PulseFadeMs) {
                f.eaten = 0;
                m_pulseTextValid[r] &= uint8_t(~(1u << (side + 1)));
                markPulseRepaint(r);
            }
            m_pulseLive += (f.change != 0) + (f.eaten != 0);
        }
    }
}

void DomModel::resetPulse()
{
    m_classifier.reset();
    clearFlashes(m_ready);
    m_pulseWarned = false;
    m_pulseLastRow = -1;
    updatePulseVersion();
}

double DomModel::pulseThreshold(bool bid) const
{
    return PulseThresholdShare * m_scales[scaleIndex(bid, false)].target;
}

void DomModel::pulseStep(qint64 nowMs, std::span<const sentinel::book::Trade> trades)
{
    using namespace sentinel::book;
    ++m_pulseSteps;
    m_pulseNow = nowMs;
    expireFlashes(nowMs);
    Snapshot snapshot{m_pulseSeries, m_tick, 0, {}, nowMs};
    m_pulseLastRow = -1;
    if (m_ready && m_tick > 0) {
        // The rows the view shows plus a margin; rows ascend by price, the model's descend.
        const int center = centerRow();
        int first = m_firstVisible < 0 ? center - PulseMaxRows / 4 : m_firstVisible - PulseMarginRows;
        int last = m_lastVisible < 0 ? center + PulseMaxRows / 4 : m_lastVisible + PulseMarginRows;
        first = std::clamp(first, 0, Rows - 1);
        last = std::clamp(last, first, std::min(Rows - 1, first + PulseMaxRows - 1));
        const int count = last - first + 1;
        for (int i = 0; i < count; ++i) m_pulseRows[i] = {m_rows[last - i].bid, m_rows[last - i].ask};
        snapshot.firstPrice = double(m_top - last) * m_tick;
        snapshot.rows = std::span<const LevelSizes>(m_pulseRows.data(), size_t(count));
        m_pulseLastRow = last;
    }
    const auto status = m_classifier.update(snapshot, trades);
    if ((status == UpdateStatus::InvalidInput || status == UpdateStatus::CapacityExceeded) && !m_pulseWarned) {
        m_pulseWarned = true; // once per series
        sLog_Warning("DOM pulse update rejected status=" << (status == UpdateStatus::InvalidInput ? "invalid_input" : "capacity_exceeded")
                     << " symbol=" << m_symbol << " tick=" << m_tick << " rows=" << snapshot.rows.size()
                     << " t=" << nowMs);
    }
    if (status == UpdateStatus::Accepted) recordPulseEvents(nowMs);
    updatePulseVersion();
    sLog_Probe("dom.pulse", "status=" << int(status) << " rows=" << snapshot.rows.size() << " trades=" << trades.size()
               << " events=" << m_classifier.events().size() << " live=" << m_pulseLive
               << " outOfInterval=" << m_classifier.outOfIntervalTradesThisUpdate()
               << " outOfIntervalTotal=" << m_classifier.outOfIntervalTradesTotal());
}

void DomModel::recordPulseEvents(qint64 nowMs)
{
    using sentinel::book::EventKind;
    for (const auto& event : m_classifier.events()) {
        const int row = m_pulseLastRow - int(event.row);
        if (row < 0 || row >= Rows) continue;
        const bool bid = event.side == sentinel::book::Side::Bid;
        if (event.size < pulseThreshold(bid)) continue;
        auto& f = m_flash[row][bid ? 0 : 1];
        if (event.kind == EventKind::Eaten) {
            m_pulseLive += f.eaten == 0;
            f.eaten = event.size;
            f.eatenAt = nowMs;
            m_pulseTextValid[row] &= uint8_t(~(1u << ((bid ? 0 : 2) + 1)));
        } else {
            m_pulseLive += f.change == 0;
            f.change = event.kind == EventKind::Added ? event.size : -event.size;
            f.changeAt = nowMs;
            m_pulseTextValid[row] &= uint8_t(~(1u << (bid ? 0 : 2)));
        }
        markPulseRepaint(row);
    }
}

void DomModel::updatePulseVersion()
{
    const auto& t = m_classifier.totals();
    const std::array<double, 8> signature{t.side[0].pulled, t.side[0].eaten, t.side[1].pulled, t.side[1].eaten,
        t.side[0].added, t.side[1].added, t.largestPull ? t.largestPull->size : 0, t.largestPull ? t.largestPull->price : 0};
    if (signature == m_pulseSignature) return;
    m_pulseSignature = signature;
    ++m_pulseVersion;
}

DomPulseCell DomModel::pulse(int row, bool bid) const
{
    DomPulseCell cell;
    if (!m_ready || row < 0 || row >= Rows || m_pulseLive == 0) return cell;
    const auto& f = m_flash[row][bid ? 0 : 1];
    const auto alpha = [&](qint64 at) { return std::clamp(1.0 - double(m_pulseNow - at) / PulseFadeMs, 0.0, 1.0); };
    if (f.change) {
        cell.change = f.change;
        cell.changeAlpha = alpha(f.changeAt);
        cell.grow = std::clamp(double(m_pulseNow - f.changeAt + PulseGrowMs / 3) / PulseGrowMs, 0.0, 1.0);
    }
    if (f.eaten) {
        cell.eaten = f.eaten;
        cell.eatenAlpha = alpha(f.eatenAt);
    }
    return cell;
}

const QString& DomModel::pulseTag(int row, bool bid, bool eaten) const
{
    static const QString none;
    if (row < 0 || row >= Rows) return none;
    const int slot = (bid ? 0 : 2) + (eaten ? 1 : 0);
    QString& text = m_pulseText[size_t(row) * 4 + slot];
    if (m_pulseTextValid[row] & (1u << slot)) return text;
    m_pulseTextValid[row] |= uint8_t(1u << slot);
    ++m_pulseTagFormats;
    const auto& f = m_flash[row][bid ? 0 : 1];
    const double value = eaten ? f.eaten : f.change;
    char buffer[PulseTagCapacity];
    int length = 0;
    if (value != 0) {
        buffer[0] = eaten ? '\xD7' : value > 0 ? '+' : '-'; // U+00D7 multiplication sign, in TagAlphabet
        const int digits = formatSize(buffer + 1, PulseTagCapacity - 1, std::abs(value));
        length = digits > 0 ? digits + 1 : 0;
    }
    // Refill in place: truncate keeps the reserved capacity of an unshared string.
    text.truncate(0);
    text.append(QLatin1StringView(buffer, length));
    return text;
}

QString DomModel::pulseSummary(PulseText form) const
{
    const auto& t = m_classifier.totals();
    const auto& bid = t.side[sentinel::book::sideIndex(sentinel::book::Side::Bid)];
    const auto& ask = t.side[sentinel::book::sideIndex(sentinel::book::Side::Ask)];
    if (form == PulseText::Tiny)
        return QStringLiteral("Ask %1/%2 · Bid %3/%4")
            .arg(sizeText(ask.pulled), sizeText(ask.eaten), sizeText(bid.pulled), sizeText(bid.eaten));
    QString text = form == PulseText::Full
        ? QStringLiteral("Asks pulled %1 eaten %2 · Bids pulled %3 eaten %4")
        : QStringLiteral("Ask pull %1 eat %2 · Bid pull %3 eat %4");
    text = text.arg(sizeText(ask.pulled), sizeText(ask.eaten), sizeText(bid.pulled), sizeText(bid.eaten));
    if (t.largestPull && m_tick > 0 && form != PulseText::NoMax)
        text += (form == PulseText::Compact ? QStringLiteral(" · max %1 @ %2") : QStringLiteral(" · biggest pull %1 @ %2"))
            .arg(sizeText(t.largestPull->size), priceText(t.largestPull->price, m_tick));
    return text;
}
