#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <functional>
#include <optional>

// GUI-thread facts shared by the active chart, DOM and paper ticket. Receive age
// is descriptive, never a silence timeout: quiet products remain healthy.
class MarketHealth : public QObject {
    Q_OBJECT
public:
    enum class Transport { Initializing, Connected, Reconnecting, Disconnected };
    enum class State { Initializing, WaitingForSubscription, WaitingForBook, WaitingForHistory,
                       Reconnecting, Stale, HistoryPartial, Unavailable, Disconnected, Live };
    struct ChartFacts {
        QString symbol;
        std::optional<bool> loading, partial, holding, unavailable;
        QString coverage, reason;
        bool operator==(const ChartFacts&) const = default;
    };
    struct Snapshot {
        QString symbol, reason, coverage;
        State state = State::Initializing;
        Transport transport = Transport::Initializing;
        bool acknowledged = false;
        std::optional<qint64> bookAgeMs, heatmapAgeMs;
        std::optional<bool> loading, partial, holding;
        QString text() const;
        QString compact() const;
    };
    explicit MarketHealth(QObject* parent = nullptr) : QObject(parent) {}
    const QString& activeSymbol() const { return m_activeSymbol; }
    void setActiveSymbol(const QString& symbol);
    void setTransport(Transport transport);
    void subscriptionRequested(const QString& symbol);
    void subscriptionAcknowledged(const QString& symbol);
    void subscriptionRefused(const QString& symbol, const QString& reason);
    void subscriptionReleased(const QString& symbol);
    void bookReceived(const QString& symbol, qint64 nowMs, bool snapshot = true);
    void bookUnavailable(const QString& symbol, const QString& reason);
    void bookStale(const QString& symbol);
    void heatmapReceived(const QString& symbol, qint64 nowMs);
    void setChartState(const ChartFacts& facts);
    // Provider reads GUI-owned snapshots/atomic renderer facts only. Consumers
    // call refreshChartState only while exposed. There is no background timer.
    void setChartProvider(std::function<ChartFacts()> provider);
    void refreshChartState();
    Snapshot snapshot(const QString& symbol, qint64 nowMs) const;
    Snapshot snapshot(const QString& symbol = {}) const;
    static QString stateText(State state);
    static QString ageText(std::optional<qint64> ageMs);
signals:
    void changed(); // semantic transition only; receive timestamps do not emit per-message
private:
    struct Facts {
        bool requested = false, acknowledged = false, bookReady = false, stale = false;
        QString refusal, bookError;
        std::optional<qint64> bookMs, heatmapMs;
    };
    QHash<QString, Facts> m_symbols;
    QString m_activeSymbol;
    Transport m_transport = Transport::Initializing;
    ChartFacts m_chart;
    std::function<ChartFacts()> m_chartProvider;
    bool m_refreshing = false;
};
