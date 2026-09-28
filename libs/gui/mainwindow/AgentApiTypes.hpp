#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <optional>

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

} // namespace AgentApi
