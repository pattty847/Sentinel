#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <optional>
#include <array>
#include <vector>

namespace AgentApi {

struct Request {
    QByteArray method;
    QString path;
    QString query;
};

struct ParseResult {
    enum class Kind { Incomplete, Complete, Error } kind = Kind::Incomplete;
    Request request;
    int status = 0;
    QString code;
    QString message;
};

struct ValidationResult {
    int status = 200;
    QString code;
    QString message;
    QString screenshotName;
    QString screenshotTarget = "main";
    qint64 startMs = 0;
    qint64 endMs = 0;
    qint64 timeframeMs = 0;
    qint64 windowMs = 60000;
    int limit = 100;
    int levels = 20;
};

struct Metadata {
    QString sessionId;
    QString symbol;
    quint64 selectionEpoch = 0;
    qint64 observedAtMs = 0;
    bool stale = false;
    QString coverage = "unknown";
    bool truncated = false;
};

struct StateSnapshot {
    Metadata meta;
    bool connected = false;
    bool serverConfigReady = false;
    QString serverHost;
    std::optional<int> serverPort;
    std::optional<QStringList> defaultSymbols;
    std::optional<QList<qint64>> configuredTimeframesMs;
    std::optional<QList<qint64>> servedTimeframesMs;
    std::optional<qint64> activeTimeframeMs;
    std::optional<int> gridWidth;
    std::optional<int> gridHeight;
    std::optional<double> tickSize;
    std::optional<double> bandPct;
    std::optional<double> candleBpsFast;
    std::optional<double> candleBpsSlow;
    std::optional<int> candleTickMultFast;
    std::optional<int> candleTickMultSlow;
    std::optional<qint64> candleSilenceMsFast;
    std::optional<qint64> candleSilenceMsSlow;
    std::optional<double> candleVolumeFast;
    std::optional<double> candleVolumeSlow;
    std::optional<double> candleTickSize;
    std::optional<qint64> heatmapReceivedAtMs;
    std::optional<qint64> candlesReceivedAtMs;
    std::optional<qint64> bookReceivedAtMs;
    std::optional<qint64> tradesReceivedAtMs;
    std::optional<bool> heatmapLayer;
    std::optional<bool> candlesLayer;
    std::optional<bool> footprintLayer;
    std::optional<bool> tpoLayer;
    std::optional<bool> volumeProfileLayer;
};

struct ViewportSnapshot {
    Metadata meta;
    std::optional<qint64> startMs;
    std::optional<qint64> endMs;
    std::optional<double> priceMin;
    std::optional<double> priceMax;
    std::optional<qint64> heatmapTimeframeMs;
    std::optional<qint64> candleTimeframeMs;
    std::optional<bool> followLive;
    std::optional<quint64> viewportVersion;
    std::optional<double> widthPx;
    std::optional<double> heightPx;
};

struct CandleRow {
    qint64 startMs = 0, endMs = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
    bool closed = false;
    qint64 seq = 0;
};
struct CandleSnapshot {
    Metadata meta;
    std::vector<CandleRow> bars;
    std::optional<qint64> nextStartMs;
};

struct BookLevel { double price = 0, qty = 0; };
struct BookSnapshot {
    Metadata meta;
    std::optional<double> bestBid, bestAsk, spread;
    std::vector<BookLevel> bids, asks;
    bool bandLimited = true;
    std::optional<double> bandMin, bandMax;
    std::optional<qint64> receivedAtMs;
    bool scanLimited = false;
};

struct TradeRow {
    qint64 receivedAtMs = 0;
    quint64 selectionEpoch = 0;
    QString id;
    QString side = "unknown";
    double price = 0, qty = 0;
};
struct TradeSummary {
    quint64 count = 0;
    double buyQty = 0, sellQty = 0, unknownQty = 0, deltaQty = 0;
    std::optional<double> vwap;
};
struct TradesSnapshot {
    Metadata meta;
    std::vector<TradeRow> trades;
    TradeSummary summary;
    bool retentionLimited = false;
};

class TradeTape {
public:
    static constexpr size_t Capacity = 10000;
    static constexpr qint64 RetentionMs = 900000;
    void append(TradeRow row);
    TradesSnapshot snapshot(Metadata meta, qint64 windowMs, size_t limit) const;
private:
    std::array<TradeRow, Capacity> m_rows{};
    size_t m_head = 0, m_count = 0;
    qint64 m_lastEvictedAtMs = 0;
    quint64 m_epoch = 0;
};

} // namespace AgentApi
