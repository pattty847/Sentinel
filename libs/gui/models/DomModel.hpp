#pragma once

#include "../../core/marketdata/model/TradeData.h"
#include <QAbstractTableModel>
#include <array>
#include <optional>

// Ingestion state, deliberately independent of the view/model and its visibility.
// Unknown sides occupy the same last-N window but never count as buy/sell/delta.
class DomTradeWindow {
public:
    static constexpr int Capacity = 1000;
    struct Entry { double price = 0; AggressorSide side = AggressorSide::Unknown; };
    void ingest(const Trade& trade);
    void clear() { m_next = m_size = 0; }
    int size() const { return m_size; }
    const auto& entries() const { return m_entries; }
private:
    std::array<Entry, Capacity> m_entries{};
    int m_next = 0;
    int m_size = 0;
};

// Small, replaceable W1 freshness policy. Time is the replica's local receive
// time, not exchange time, paint time or time of a trade. No timer is owned here.
struct DomFreshness {
    static constexpr qint64 StaleAfterMs = 3000;
    std::optional<bool> connected;
    bool snapshotStale = false;
    bool awaitingBook = false;
    qint64 receiveMs = 0;
    QString text(qint64 nowMs) const;
};

// GUI-thread only. Retained, contiguous integer buckets (including empty rows).
// A bounded 2,001-tick inspection window avoids memory/work proportional to the
// replica's potentially huge band. Manual mode pins this price window.
class DomModel : public QAbstractTableModel {
public:
    static constexpr int Rows = 2001;
    enum Column { Bid, Price, Ask, Buys, Sells, Delta, Columns };
    enum Role { QuantityRole = Qt::UserRole, BucketRole, BestSideRole };
    explicit DomModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void clear(const QString& symbol);
    void publish(const LiveOrderBook& book, const DomTradeWindow& trades, bool follow);
    int centerRow() const;
    double tick() const { return m_tick; }
    double maxBid() const { return m_maxBid; }
    double maxAsk() const { return m_maxAsk; }
    QString summary() const;
    QString executionSummary() const;
    QString aggregation() const;
    static QString priceText(double price, double tick);
private:
    struct Row {
        double bid = 0, ask = 0;
        int buys = 0, sells = 0;
        bool bestBid = false, bestAsk = false;
        bool operator==(const Row&) const = default;
    };
    static std::optional<qint64> bucket(double price, double tick);
    QString m_base = "base", m_quote = "quote";
    double m_tick = 0;
    qint64 m_top = 0, m_center = 0;
    bool m_ready = false;
    std::array<Row, Rows> m_rows{};
    std::vector<std::pair<uint32_t, double>> m_bidBuffer, m_askBuffer;
    double m_bestBid = 0, m_bestAsk = 0, m_maxBid = 1, m_maxAsk = 1;
    int m_trades = 0, m_unknown = 0;
};
