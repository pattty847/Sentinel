// Dedicated QThread processor for remote heatmap slices; server is authoritative for columns.
#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QVector>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../datasources/IGridDataSource.hpp"

class FootprintStreamState;

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();

public slots:
    void onHeatmapSliceReceived(const HeatmapSlice& slice);
    void onFootprintSliceReceived(const FootprintSlice& slice);
    void onHeatmapHistoryReceived(const QString& symbol,
                                  int64_t timeframeMs,
                                  int gridWidth,
                                  int gridHeight,
                                  const QVector<IGridDataSource::HeatmapHistoryColumn>& columns);
    
public:
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
    void setCacheCapacityOverride(int capacity);
    void setServerTimeframe(int64_t timeframeMs);
    
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
    void footprintColumnReady(int x, int gridWidth, int gridHeight, QByteArray columnQ16);

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
    
    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;
    int64_t m_forcedTimeframeMs = 0;
    
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
    std::atomic<bool> m_shuttingDown{false};

    // Week 0: writes disabled to prevent unbounded client growth; reserved for future bounded cache design.
    std::unordered_map<HeatmapGridKey, HeatmapColumnCache, HeatmapGridKeyHash, HeatmapGridKeyEq> m_heatmapCache;
    int m_cacheCapacityOverride = 0;
    int m_footprintGridWidth = 5120;
    int m_footprintGridHeight = 2048;
    std::unique_ptr<FootprintStreamState> m_footprintStream;

};
