#pragma once

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include "../protocol/HeatmapSlice.hpp"
#include "IHeatmapDataSource.hpp"
#include "HeatmapColumnStore.hpp"
#include "../config/ConfigTypes.hpp"

#include <memory>

class HeatmapTwapStreamer : public QObject {
    Q_OBJECT
public:
    explicit HeatmapTwapStreamer(IHeatmapDataSource& model,
                                 const ServerHeatmapConfig& config,
                                 QObject* parent = nullptr);

    void start();
    void stop();

    // Phase 2: prime the in-RAM HistoryRing for `symbol` on the active timeframe
    // from the most recent disk-resident records. Must be called BEFORE start().
    // Returns the number of columns primed (0 if persistence is disabled, the
    // symbol has no on-disk history, or the recovered gridHeight does not match
    // the current configured gridHeight).
    int primeRingFromDisk(const std::string& symbol);

    // Convenience: call primeRingFromDisk for every symbol. Safe to call when
    // persistence is disabled (no-op in that case).
    int bootstrapFromDisk(const std::vector<std::string>& symbols);

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
                      std::vector<HistoryColumn>& out,
                      int64_t startTimeMs = 0) const;

    // Phase 4: lowest bucketStartMs persisted on disk for (symbol, tf), or 0
    // if persistence is disabled or no records exist. Used to populate the
    // protocol's oldest_available_ms hint.
    int64_t oldestPersistedMs(const std::string& symbol, int64_t timeframeMs) const;

signals:
    void heatmapSliceReady(const HeatmapSlice& slice);

private:
    enum class NormalizeMode { Linear, Log, Power };

    struct IntensityConfig {
        NormalizeMode mode = NormalizeMode::Log;
        bool useRunningMax = true;
        double runningMaxDecay = 0.995;
        double logScale = 1000.0;
        double powerExp = 0.4;
        double intensityFloor = 0.001;
    };

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
        int64_t observedMs = 0;
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
    void emitFormingBucket(const std::string& symbol,
                           SymbolState& state,
                           const TimeframeState& frame,
                           int64_t nowMs,
                           double lastTrade);
    void storeHistory(const std::string& symbol,
                      SymbolState& state,
                      int64_t timeframeMs,
                      const QByteArray& column,
                      const QByteArray& liquidityColumn,
                      double liquidityScale,
                      double minPrice,
                      double maxPrice,
                      double tickSize,
                      int64_t bucketStartMs,
                      int64_t bucketEndMs);
    void persistColumn(const std::string& symbol,
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
                                       const std::vector<double>& askValues,
                                       bool updateRunningMax = true);
    QByteArray toLiquidityColumn(const std::vector<double>& bidValues,
                                 const std::vector<double>& askValues,
                                 double& outScale) const;

    static int64_t alignBucketStart(int64_t nowMs, int64_t timeframeMs);
    double bandForTimeframe(int64_t timeframeMs) const;
    void applyBandRange(SymbolState& state, double midPrice, int64_t timeframeMs);
    IntensityConfig parseIntensityConfig() const;

    IHeatmapDataSource& m_model;
    ServerHeatmapConfig m_config;
    IntensityConfig m_intensity;
    QTimer m_timer;
    int m_sampleMs = 50;
    // Sample gaps longer than this are treated as unobserved, not integrated.
    static constexpr int64_t kMaxSampleGapMs = 5000;

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

    std::unique_ptr<HeatmapColumnStore> m_columnStore; // null when persistence disabled or lock failed
};
