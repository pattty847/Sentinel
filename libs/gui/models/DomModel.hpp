#pragma once

#include "../../core/marketdata/model/TradeData.h"
#include <QAbstractTableModel>
#include <array>
#include <optional>

// Ingestion state, deliberately independent of the view/model and its visibility.
// A fixed ring of the latest executions with size and exchange trade time. The
// model reads the trailing WindowMs of it; when the ring is full and its oldest
// entry is newer than the window start, the model reports the shorter span.
// Unknown sides occupy the ring but never count as buy/sell/delta.
class DomTradeWindow {
public:
    static constexpr int Capacity = 8192;
    static constexpr qint64 WindowMs = 60000;
    struct Entry {
        double price = 0, size = 0;
        qint64 timeMs = 0; // 0: the trade carried no exchange time
        AggressorSide side = AggressorSide::Unknown;
    };
    void ingest(const Trade& trade);
    void clear() { m_next = m_size = 0; }
    int size() const { return m_size; }
    const auto& entries() const { return m_entries; }
    // Exchange time of the oldest retained entry; 0 when empty.
    qint64 oldestMs() const { return m_size ? m_entries[m_size < Capacity ? 0 : m_next].timeMs : 0; }
private:
    std::array<Entry, Capacity> m_entries{};
    int m_next = 0;
    int m_size = 0;
};

// Replaceable W1 connection-based freshness policy. Book-change age is separate
// from transport liveness; a quiet connected market is not a stale feed.
struct DomFreshness {
    std::optional<bool> connected;
    bool snapshotStale = false;
    bool awaitingBook = false;
    qint64 lastChangeMs = 0;
    QString text(qint64 nowMs) const;
};

// GUI-thread only. Retained, contiguous integer buckets (including empty rows).
// A bounded 2,001-tick inspection window avoids memory/work proportional to the
// replica's potentially huge band. Manual mode pins this price window.
// Bars scale to the rows the view reports visible, through a smoothed maximum.
class DomModel : public QAbstractTableModel {
public:
    static constexpr int Rows = 2001;
    static constexpr double ScaleTauMs = 90; // log-domain decay of the bar scale: settles in ~300-500 ms
    enum Column { Bid, Price, Ask, Buys, Sells, Delta, Columns };
    enum Role { QuantityRole = Qt::UserRole, BucketRole, BestSideRole, CumulativeRole };
    explicit DomModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void clear(const QString& symbol);
    // nowMs < 0 reads the wall clock. It anchors the trade window and the scale decay.
    void publish(const LiveOrderBook& book, const DomTradeWindow& trades, bool follow, qint64 nowMs = -1);
    int centerRow() const;
    double tick() const { return m_tick; }
    // Paint-path reads without QVariant/QString work.
    bool isBest(int row, bool bid) const { return m_ready && row >= 0 && row < Rows && (bid ? m_rows[row].bestBid : m_rows[row].bestAsk); }
    double size(int row, bool bid, bool cumulativeSeries) const;
    // Rows the view shows; bars scale to these. A negative bound means every row.
    void setVisibleRows(int first, int last);
    // Cumulative mode shows size summed from the touch outward in Bid/Ask.
    void setCumulative(bool on);
    bool cumulative() const { return m_cumulative; }
    // Square-root bar length in [0, 1] against the smoothed visible maximum of that series.
    double barFraction(double value, bool bid, bool cumulativeSeries) const;
    double scaleMax(bool bid, bool cumulativeSeries) const { return m_scales[scaleIndex(bid, cumulativeSeries)].shown; }
    // True while a bar scale is still settling or a trade leaves the window by nowMs.
    bool needsRepublish(qint64 nowMs) const;
    QString summary(bool compact = false) const;
    QString executionSummary() const;
    QString aggregation(bool compact = false) const;
    static QString priceText(double price, double tick);
    // Three significant digits, leading zero dropped, k/m/b suffixes: .634, 5.87, 13.6, 1.36k.
    static QString sizeText(double value);
    // Tooltip text: the precision of the server's float sizes, without float noise.
    static QString fullSizeText(double value);
private:
    struct Row {
        double bid = 0, ask = 0, cumBid = 0, cumAsk = 0;
        double buys = 0, sells = 0;
        bool bestBid = false, bestAsk = false;
        bool operator==(const Row&) const = default;
    };
    struct Scale {
        double shown = 0, target = 0;
    };
    static int scaleIndex(bool bid, bool cumulativeSeries) { return (bid ? 0 : 1) + (cumulativeSeries ? 2 : 0); }
    static std::optional<qint64> bucket(double price, double tick);
    void accumulateDepth(const LiveOrderBook& book, qint64 origin);
    void addTrades(const DomTradeWindow& trades, const LiveOrderBook::DenseBookSnapshotView& view,
                   qint64 origin, qint64 nowMs);
    bool updateScales(qint64 nowMs);
    QString m_base = "base", m_quote = "quote", m_symbol = "this product";
    double m_tick = 0;
    qint64 m_top = 0, m_center = 0;
    bool m_ready = false, m_cumulative = false;
    QString m_aggregationIssue;
    std::array<Row, Rows> m_rows{}, m_next{};
    std::vector<std::pair<uint32_t, double>> m_bidBuffer, m_askBuffer;
    std::array<Scale, 4> m_scales{};
    int m_firstVisible = -1, m_lastVisible = -1;
    bool m_visibleChanged = false;
    qint64 m_lastScaleMs = 0, m_nextExpiryMs = 0;
    double m_bestBid = 0, m_bestAsk = 0;
    int m_windowTrades = 0, m_unknown = 0, m_untimed = 0;
    qint64 m_coveredMs = DomTradeWindow::WindowMs;
};
