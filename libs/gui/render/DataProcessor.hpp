/*
Sentinel — DataProcessor
Role: Receives remote heatmap slices and forwards columns to the GPU renderer.
Inputs/Outputs: Takes heatmap slice payloads; emits column and range reset signals.
Threading: Lives and operates on a dedicated QThread; receives data from main and signals back.
Performance: Minimal work to keep upload cadence smooth.
Integration: Owned by UnifiedGridRenderer; GPU-only heatmap path.
Observability: Logs ingest diagnostics via sLog_Render when enabled.
Related: DataProcessor.cpp, UnifiedGridRenderer.h, GridViewState.hpp.
Assumptions: Server is authoritative for heatmap columns.
*/
#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QVector>
#include <atomic>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include "../datasources/IGridDataSource.hpp"

class GridViewState;

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();

public slots:
    // Data ingestion (slots for cross-thread invocation)
    void onHeatmapSliceReceived(const QString& symbol,
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
                                const QString& format,
                                const QByteArray& column,
                                const QByteArray& liquidityColumn,
                                double liquidityScale,
                                bool reset);
    void onHeatmapHistoryReceived(const QString& symbol,
                                  int64_t timeframeMs,
                                  int gridWidth,
                                  int gridHeight,
                                  const QVector<IGridDataSource::HeatmapHistoryColumn>& columns);
    
public:
    
    // Configuration
    void setGridViewState(GridViewState* viewState) { m_viewState = viewState; }
    
    // Control
    void clearData();
    void startProcessing();
    void stopProcessing();
    
    void setPriceResolution(double resolution);
    double getPriceResolution() const;
    void addTimeframe(int timeframe_ms);
    int64_t suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const;
    int getDisplayMode() const;

    void setTimeframe(int timeframe_ms);
    bool isManualTimeframeSet() const;

    void setHeatmapGridHeight(int height);
    void setHeatmapGridDimensions(int width, int height);
    void setHeatmapIntensityScale(double scale);
    void setHeatmapRecenterFraction(double fraction);
    
signals:
    void heatmapColumnReady(int64_t sliceStartMs,
                            int64_t sliceEndMs,
                            int64_t timeframeMs,
                            double minPrice,
                            double maxPrice,
                            double tickSize,
                            const QByteArray& column,
                            const QByteArray& liquidityColumn,
                            double liquidityScale,
                            int intensityBytesPerCell);
    void heatmapRangeReset(double minPrice, double maxPrice, double tickSize, int gridWidth, int gridHeight);

private:
    struct HeatmapGridKey {
        std::string symbol;
        int gridWidth = 0;
        int gridHeight = 0;
        int64_t timeframeMs = 0;
        double minPrice = 0.0;
        double maxPrice = 0.0;
        double tickSize = 0.0;
    };

    struct HeatmapGridKeyHash {
        size_t operator()(const HeatmapGridKey& key) const noexcept;
    };

    struct HeatmapGridKeyEq {
        bool operator()(const HeatmapGridKey& a, const HeatmapGridKey& b) const noexcept;
    };

    struct HeatmapColumnCache {
        int capacity = 0;
        int writeIndex = 0;
        int count = 0;
        std::vector<IGridDataSource::HeatmapHistoryColumn> columns;

        void reset(int newCapacity);
        void push(IGridDataSource::HeatmapHistoryColumn column);
    };
    
    // Components
    GridViewState* m_viewState = nullptr;
    
    // Manual timeframe management
    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;
    
    
    // GPU heatmap rasterization config
    int m_heatmapGridWidth = 5120;
    int m_heatmapGridHeight = 2048;
    double m_heatmapIntensityScale = 1.0;
    double m_heatmapMinPrice = 0.0;
    double m_heatmapMaxPrice = 0.0;
    double m_heatmapTickSize = 0.0;
    bool m_heatmapRangeValid = false;
    double m_heatmapRecenterFraction = 0.15;
    int64_t m_heatmapLastSliceStart = std::numeric_limits<int64_t>::min();
    QByteArray m_heatmapLastColumn;
    bool m_heatmapHasLastColumn = false;
    // Shutdown flag to prevent processing after stopProcessing() is called
    std::atomic<bool> m_shuttingDown{false};

    std::unordered_map<HeatmapGridKey, HeatmapColumnCache, HeatmapGridKeyHash, HeatmapGridKeyEq> m_heatmapCache;
    int m_cacheCapacityOverride = 0;

};
