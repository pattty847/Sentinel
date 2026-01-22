#pragma once

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <unordered_map>
#include <vector>
#include <string>

class ServerDataModel;

class HeatmapTwapStreamer : public QObject {
    Q_OBJECT
public:
    explicit HeatmapTwapStreamer(ServerDataModel& model, QObject* parent = nullptr);

    void start();
    void stop();

signals:
    void heatmapSliceReady(const QString& symbol,
                           int64_t bucketStartMs,
                           int64_t bucketEndMs,
                           int64_t timeframeMs,
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
        int height = 0;
        int64_t lastSampleMs = 0;
        bool initialized = false;
        bool pendingReset = false;
        std::vector<double> rowValuesBid;
        std::vector<double> rowValuesAsk;
        std::vector<TimeframeState> frames;
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
    QByteArray toIntensityColumnSigned(const std::vector<double>& bidValues,
                                       const std::vector<double>& askValues) const;
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

    int m_defaultHeight = 2048;
    double m_defaultTickSize = 1.0;
    int64_t m_activeTimeframeMs = 100;

    std::vector<int64_t> m_timeframesMs;
    std::unordered_map<std::string, SymbolState> m_symbols;
};
