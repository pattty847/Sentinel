#pragma once

#include "../../core/marketdata/model/TradeData.h"
#include "../../core/marketdata/analytics/BookDiffClassifier.hpp"
#include <QAbstractTableModel>
#include <array>
#include <optional>
#include <span>

// Ingestion state, deliberately independent of the view/model and its visibility.
// A fixed ring of the latest executions with size and exchange trade time. The
// model reads the trailing WindowMs of it; when the ring is full and its oldest
// entry is newer than the window start, the model reports the shorter span.
// Unknown sides occupy the ring but never count as buy/sell/delta.
class DomTradeWindow {
public:
    static constexpr int Capacity = 8192;
    static constexpr qint64 WindowMs = 60000;
    // Exchange stamps may run slightly ahead of this clock; beyond this a trade waits for its time.
    static constexpr qint64 SkewMs = 1000;
    // The one eligibility rule for volumes and the summary: nowMs - WindowMs < t <= nowMs + SkewMs.
    static bool inWindow(qint64 timeMs, qint64 nowMs)
    {
        return timeMs > 0 && timeMs > nowMs - WindowMs && timeMs <= nowMs + SkewMs;
    }
    struct Entry {
        double price = 0, size = 0;
        qint64 timeMs = 0; // 0: the trade carried no exchange time
        AggressorSide side = AggressorSide::Unknown;
    };
    void ingest(const Trade& trade);
    void clear() { m_next = m_size = 0; m_evictedMaxMs = 0; }
    int size() const { return m_size; }
    const auto& entries() const { return m_entries; }
    // Latest exchange time among evicted entries (0: none). Volume at or before it may be
    // missing, whatever order or timestamps the retained entries have.
    qint64 evictedMaxMs() const { return m_evictedMaxMs; }
private:
    std::array<Entry, Capacity> m_entries{};
    qint64 m_evictedMaxMs = 0;
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

// Liquidity Pulse display state of one side of one row: the latest added (+) or
// pulled (-) size and the latest eaten size, each with its alpha in [0, 1].
struct DomPulseCell {
    double change = 0, eaten = 0;
    double changeAlpha = 0, eatenAlpha = 0;
    double grow = 1; // share of an added segment drawn so far, in (0, 1]
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
    // Row tick: group x native tick (the price column's grid).
    double tick() const { return m_tick; }
    double nativeTick() const { return m_nativeTick; }
    // Grouping: each row sums `group` native levels on the absolute grid
    // [k * group, (k + 1) * group) (DomGrouping.hpp). Applied at the next publish,
    // which re-bins every row; a pinned window keeps the price on screen.
    void setGroup(int64_t group);
    int64_t group() const { return m_group; }
    int64_t requestedGroup() const { return m_requestedGroup; }
    // Mid of the last published native top (0 without one).
    double mid() const { return m_bestBid > 0 && m_bestAsk > 0 ? (m_bestBid + m_bestAsk) / 2 : m_bestBid + m_bestAsk; }
    // Grouped rows are filled only around the visible rows: a margin of GroupMarginRows each
    // side, widened to GroupBudgetLevels native levels; at group 1 the whole window.
    static constexpr int GroupMarginRows = 32;
    static constexpr int64_t GroupBudgetLevels = 4096;
    // Test hooks: the filled row range and the native levels the last publication read.
    int filledFirst() const { return m_rangeFirst; }
    int filledLast() const { return m_rangeLast; }
    qint64 lastGroupLevels() const { return m_groupLevels; }
    // Paint-path reads without QVariant/QString work.
    bool isBest(int row, bool bid) const { return m_ready && row >= 0 && row < Rows && (bid ? m_rows[row].bestBid : m_rows[row].bestAsk); }
    double size(int row, bool bid, bool cumulativeSeries) const;
    // Replica entries the last publication summed beyond the window edge (test hook).
    qint64 lastDepthSteps() const { return m_depthSteps; }
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
    // Positive dust below 1e-4 shows "<.0001".
    static QString sizeText(double value);
    // Signed (delta) form: +1.25, -.5; magnitudes below 1e-4, including exact cancellation, show "≈0".
    static QString signedSizeText(double value);
    // Tooltip text: the precision of the server's float sizes, without float noise.
    static QString fullSizeText(double value);
    // sizeText into a Latin-1 buffer without allocating; returns the length (0 if it does not fit).
    static int formatSize(char* out, int capacity, double value);
    // Every character formatSize can emit (exponent form included: 7.1e+03b), plus the
    // tag prefixes. Glyph strips that draw tags must cover all of it.
    static constexpr char TagAlphabet[] = "0123456789.+-e<kmb\xD7";

    // Liquidity Pulse. pulseStep classifies the rows just published (visible rows
    // plus a margin) against the previous step with trades stamped on receipt, all
    // in one monotonic clock; call it on every display tick, after publish.
    static constexpr qint64 PulseFadeMs = 1200;
    static constexpr qint64 PulseGrowMs = 200;
    static constexpr qint64 PulseWindowMs = 10000;
    static constexpr int PulseMaxRows = 256;
    static constexpr int PulseMarginRows = 8;
    // Flashes below this share of the side's visible maximum level are not shown.
    static constexpr double PulseThresholdShare = 0.05;
    void pulseStep(qint64 monotonicMs, std::span<const sentinel::book::Trade> trades);
    // Fresh baseline: no flashes, no totals; the next step only observes.
    void resetPulse();
    DomPulseCell pulse(int row, bool bid) const;
    // Tag text ("+1.2", "-4.2", "×.5"), formatted on first use after its flash starts,
    // in place into per-slot strings whose capacity is reserved at construction.
    static constexpr int PulseTagCapacity = 16;
    const QString& pulseTag(int row, bool bid, bool eaten) const;
    double pulseThreshold(bool bid) const;
    const sentinel::book::RollingTotals& pulseTotals() const { return m_classifier.totals(); }
    // Changes whenever the footer's numbers change.
    uint64_t pulseTotalsVersion() const { return m_pulseVersion; }
    // Footer text, longest first: Full, Compact, NoMax (the biggest pull only in the
    // tooltip), Tiny ("Ask 6.8/1.1 · Bid .4/2", pulled/eaten).
    enum class PulseText { Full, Compact, NoMax, Tiny };
    QString pulseSummary(PulseText form = PulseText::Full) const;
    uint64_t pulseOutOfIntervalTotal() const { return m_classifier.outOfIntervalTradesTotal(); }
    // Test hooks: steps classified and the events the last step produced (before the threshold).
    qint64 pulseSteps() const { return m_pulseSteps; }
    size_t lastPulseEvents() const { return m_classifier.events().size(); }
    qint64 pulseTagFormats() const { return m_pulseTagFormats; }
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
    qint64 anchoredTop(double nativeTick, int64_t group) const;
    void groupRange(int64_t group);
    void fillRows(const LiveOrderBook& book, qint64 origin, int64_t group);
    void withdrawScaleWork();
    uint8_t textChanges(const Row& before, const Row& after, bool moved) const;
    const QString& cachedText(int row, int col) const;
    void addTrades(const DomTradeWindow& trades, const LiveOrderBook::DenseBookSnapshotView& view,
                   qint64 origin, qint64 nowMs);
    bool updateScales(qint64 nowMs);
    QString m_base = "base", m_quote = "quote", m_symbol = "this product";
    double m_tick = 0, m_nativeTick = 0;
    int64_t m_group = 1, m_requestedGroup = 1;
    int m_rangeFirst = 0, m_rangeLast = Rows - 1;
    qint64 m_groupLevels = 0;
    qint64 m_top = 0, m_center = 0;
    bool m_ready = false, m_cumulative = false;
    QString m_aggregationIssue;
    std::array<Row, Rows> m_rows{}, m_next{};
    std::vector<std::pair<uint32_t, double>> m_bidBuffer, m_askBuffer;
    std::array<Scale, 4> m_scales{};
    int m_firstVisible = -1, m_lastVisible = -1;
    bool m_visibleChanged = false;
    // Depth beyond a pinned window edge, reused while book, grid and window are unchanged.
    struct DepthKey {
        uint64_t version = 0;
        qint64 origin = 0, rangeTop = 0, rangeBottom = 0, bestBid = 0, bestAsk = 0;
        double tick = 0;
        int64_t group = 1;
        bool operator==(const DepthKey&) const = default;
    };
    std::optional<DepthKey> m_depthKey;
    double m_outsideBid = 0, m_outsideAsk = 0;
    qint64 m_depthSteps = 0;
    // Display strings per cell, formatted on first paint after their value changes.
    mutable std::array<QString, Rows * Columns> m_text;
    mutable std::array<uint8_t, Rows> m_textValid{}; // bit per column
    qint64 m_lastScaleMs = 0, m_nextExpiryMs = 0;
    double m_bestBid = 0, m_bestAsk = 0;
    int m_windowTrades = 0, m_unknown = 0, m_untimed = 0, m_ahead = 0;
    qint64 m_coveredMs = DomTradeWindow::WindowMs;

    struct PulseFlash {
        double change = 0, eaten = 0; // change > 0 added, < 0 pulled
        qint64 changeAt = 0, eatenAt = 0;
    };
    void clearFlashes(bool repaint);
    void shiftFlashes(qint64 rows);
    void expireFlashes(qint64 nowMs);
    void recordPulseEvents(qint64 nowMs);
    bool pulseRepaint(int row) const;
    void markPulseRepaint(int row);
    void updatePulseVersion();
    sentinel::book::BookDiffClassifier m_classifier{{PulseMaxRows, 2048, PulseWindowMs, 0}};
    std::array<sentinel::book::LevelSizes, PulseMaxRows> m_pulseRows{};
    std::array<std::array<PulseFlash, 2>, Rows> m_flash{};
    mutable std::array<QString, Rows * 4> m_pulseText;
    mutable std::array<uint8_t, Rows> m_pulseTextValid{}; // bit per (side, slot)
    uint64_t m_pulseSeries = 1, m_pulseVersion = 0;
    std::array<double, 8> m_pulseSignature{};
    qint64 m_pulseNow = 0, m_pulseSteps = 0;
    mutable qint64 m_pulseTagFormats = 0;
    int m_pulseLastRow = -1;
    int m_pulseLive = 0;
    int m_repaintFirst = Rows, m_repaintLast = -1;
    bool m_pulseWarned = false;
};
