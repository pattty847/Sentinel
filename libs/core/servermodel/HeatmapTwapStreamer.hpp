#pragma once

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

class ServerDataModel;

class HeatmapTwapStreamer : public QObject {
    Q_OBJECT
public:
    explicit HeatmapTwapStreamer(ServerDataModel& model, QObject* parent = nullptr);

    void start();
    void stop();

    struct HistoryColumn {
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
        QByteArray intensity;
        QByteArray liquidity;
        double liquidityScale = 1.0;
    };

    bool fetchHistory(const std::string& symbol,
                      int64_t timeframeMs,
                      int64_t endTimeMs,
                      int count,
                      int& outGridWidth,
                      int& outGridHeight,
                      std::vector<HistoryColumn>& out) const;

signals:
    void heatmapSliceReady(const QString& symbol,
                           int64_t bucketStartMs,
                           int64_t bucketEndMs,
                           int64_t timeframeMs,
                           int gridWidth,
                           int gridHeight,
                           double minPrice,
                           double maxPrice,
                           double tickSize,
                           double midPrice,
                           double lastTrade,
                           const QByteArray& column,
                           const QByteArray& liquidityColumn,
                           double liquidityScale,
                           bool reset);

private:
    struct HistoryRing {
        int capacity = 0;
        int writeIndex = 0;
        int count = 0;
        std::vector<HistoryColumn> columns;
    };

    struct TimeframeState {
        int64_t timeframeMs = 0;
        int64_t bucketStartMs = 0;
        int64_t bucketEndMs = 0;
        std::vector<double> accumBid;
        std::vector<double> accumAsk;
    };

    struct SymbolState {
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 1.0;
        double lastMidPrice = 0.0;
        double lastRecenterMid = 0.0;
        double runningMaxBid = 0.0;
        double runningMaxAsk = 0.0;
        int height = 0;
        int64_t lastSampleMs = 0;
        bool initialized = false;
        bool pendingReset = false;
        std::vector<double> rowValuesBid;
        std::vector<double> rowValuesAsk;
        std::vector<TimeframeState> frames;
        std::unordered_map<int64_t, HistoryRing> historyByTf;
    };

    void onSample();
    void ensureSymbolState(const std::string& symbol, SymbolState& state, double midPrice);
    void accumulateForSymbol(const std::string& symbol,
                             SymbolState& state,
                             int64_t nowMs,
                             double midPrice,
                             double lastTrade);
    void finalizeBucket(const std::string& symbol,
                        SymbolState& state,
                        TimeframeState& frame,
                        double lastTrade,
                        double midPrice);
    void storeHistory(SymbolState& state,
                      int64_t timeframeMs,
                      const QByteArray& column,
                      const QByteArray& liquidityColumn,
                      double liquidityScale,
                      double minPrice,
                      double maxPrice,
                      double tickSize,
                      int64_t bucketStartMs,
                      int64_t bucketEndMs);
    QByteArray toIntensityColumnSigned(SymbolState& state,
                                       const std::vector<double>& bidValues,
                                       const std::vector<double>& askValues);
    QByteArray toLiquidityColumn(const std::vector<double>& bidValues,
                                 const std::vector<double>& askValues,
                                 double& outScale) const;

    static int64_t alignBucketStart(int64_t nowMs, int64_t timeframeMs);
    double bandForTimeframe(int64_t timeframeMs) const;
    void applyBandRange(SymbolState& state, double midPrice, int64_t timeframeMs);

    ServerDataModel& m_model;
    QTimer m_timer;
    int m_sampleMs = 50;

    double m_recenterDelta = 0.01;
    double m_bandFast = 0.15;
    double m_bandMedium = 0.25;
    double m_bandSlow = 0.35;

    int m_defaultWidth = 5120;
    int m_defaultHeight = 2048;
    double m_defaultTickSize = 1.0;
    bool m_fixedTickSize = true;
    int64_t m_activeTimeframeMs = 100;

    std::vector<int64_t> m_timeframesMs;
    std::unordered_map<std::string, SymbolState> m_symbols;
    mutable std::mutex m_historyMutex;
};
