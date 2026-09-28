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
#include "HeatmapColumnWindow.hpp"

class FootprintStreamState;
class TpoStreamState;
#include "VolumeProfileState.hpp"   // for VolumeProfileState::Snapshot in signal
struct VolumeProfileSlice;

class DataProcessor : public QObject {
    Q_OBJECT
    
public:
    explicit DataProcessor(QObject* parent = nullptr);
    ~DataProcessor();

public slots:
    void setActiveSymbol(const QString& symbol);
    void onHeatmapSliceReceived(const HeatmapSlice& slice);
    void onFootprintSliceReceived(const FootprintSlice& slice);
    void onTpoSliceReceived(const TpoSlice& slice);
    void onVolumeProfileSliceReceived(const VolumeProfileSlice& slice);
    void onHeatmapHistoryReceived(const QString& symbol,
                                  int64_t timeframeMs,
                                  int gridWidth,
                                  int gridHeight,
                                  int64_t requestEndMs,
                                  int64_t oldestAvailableMs,
                                  const QVector<IGridDataSource::HeatmapHistoryColumn>& columns);
    // Visible time range from the GUI; places the heatmap window (INV-045).
    void setHeatmapViewport(qint64 viewStartMs, qint64 viewEndMs, bool follow);
    
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
    void setServerTimeframe(int64_t timeframeMs);
    
signals:
    // Ring writes for the GPU window; live and history both arrive this way.
    void heatmapWindowUpdated(heatmap_window::UpdatePtr update);
    void heatmapHistoryFetchNeeded(qint64 timeframeMs, qint64 endTimeMs, int count);
    void heatmapHistoryStatus(bool loading, qint64 oldestAvailableMs);
    void heatmapRangeReset(double minPrice, double maxPrice, double tickSize, int gridWidth, int gridHeight);
    void footprintColumnReady(int x, int gridWidth, int gridHeight, QByteArray columnQ16);
    void tpoColumnReady(int x,
                        int gridWidth,
                        int gridHeight,
                        QByteArray letters,
                        int64_t sessionStartMs,
                        int64_t sessionEndMs,
                        int64_t timeframeMs);
    // Emitted after session data changes; row indices in grid space (0 = highest price).
    // maxPrice and tickSize let the receiver convert row → price without heatmap coupling.
    void tpoPocVahValReady(int pocRow, int vahRow, int valRow,
                           int gridHeight,
                           double maxPrice, double tickSize);
    void volumeProfileReady(std::vector<float> bins, VolumeProfileState::Snapshot snap);

private:
    struct HeatmapViewKey {
        int64_t startBucket = std::numeric_limits<int64_t>::min();
        int64_t endBucket = std::numeric_limits<int64_t>::min();
        bool follow = false;
    };

    void ensureHeatmapWindow(int64_t timeframeMs, int width, int rows);
    void resetHeatmapWindow();
    void publishHeatmapWindow(std::shared_ptr<heatmap_window::Update> update, bool firstPlacement);
    void requestHeatmapFetch();

    heatmap_window::ColumnWindow m_heatmapWindow;
    HeatmapViewKey m_lastHeatmapView;
    bool m_heatmapFetchInFlight = false;
    int64_t m_heatmapFetchEndMs = 0;
    int m_heatmapFetchCount = 0;
    uint64_t m_heatmapFetchGeneration = 0;

    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;
    int64_t m_forcedTimeframeMs = 0;
    
    int m_heatmapGridWidth = 5120;
    int m_heatmapGridHeight = 2048;
    double m_heatmapIntensityScale = 1.0;
    std::atomic<bool> m_shuttingDown{false};
    QString m_activeSymbol;

    int m_footprintGridWidth = 5120;
    int m_footprintGridHeight = 2048;
    std::unique_ptr<FootprintStreamState> m_footprintStream;
    int m_tpoGridWidth = 5120;
    int m_tpoGridHeight = 2048;
    double m_tpoMaxPrice = 0.0;
    double m_tpoTickSize = 0.0;
    std::unique_ptr<TpoStreamState> m_tpoStream;
    std::unique_ptr<VolumeProfileState> m_vpStream;

};
