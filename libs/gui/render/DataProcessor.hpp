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
#include "RecordingBandPolicy.hpp"
#include <QTimer>

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
    void setHeatmapViewport(qint64 viewStartMs, qint64 viewEndMs, bool follow,
                            double minPrice = 0, double maxPrice = 0,
                            double widthPx = 0, double heightPx = 0);
    void setRecordingConfig(bool requested, double minRowPx, double aspect);
    void setRecordingCapability(bool available);
    void setRecordingConnected(bool connected);
    void refreshRecordingHistory();
    void onRecordingHistoryReceived(const SentinelStreamClient::RecordingHistoryPage& page);
    void onRecordingHistoryError(const QString& symbol, const QString& requestId,
                                 uint64_t generation, const QString& message);
    
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
    heatmap_window::WallsSnapshot captureHeatmapWalls(const heatmap_window::WallQuery& query) const;
    
signals:
    // Ring writes for the GPU window; live and history both arrive this way.
    void heatmapWindowUpdated(heatmap_window::UpdatePtr update);
    void recordingHistoryFetchNeeded(const protocol::recordingwire::Request& request);
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
        double minPrice = 0, maxPrice = 0, widthPx = 0, heightPx = 0;
    };

    void ensureHeatmapWindow(int64_t timeframeMs, int width, int rows);
    void resetHeatmapWindow();
    void publishHeatmapWindow(std::shared_ptr<heatmap_window::Update> update, bool firstPlacement);
    void requestHeatmapFetch();

    bool recordingMode() const { return m_recordingRequested && m_recordingAvailable; }
    void scheduleRecordingBand();
    void applyRecordingBand();
    void sendRecordingRequest(int64_t endMs);
    void resetRecordingRequest();
    bool m_recordingRequested = false, m_recordingAvailable = false;
    bool m_recordingConnected = false, m_recordingBootstrapped = false;
    bool m_recordingInFlight = false, m_recordingBandConfirmed = false;
    uint64_t m_bandGeneration = 0, m_recordingSerial = 0;
    QString m_recordingRequestId;
    int64_t m_recordingEndMs = 0;
    double m_recordingMinRowPx = 2, m_recordingAspect = 0.75;
    recording_view::View m_recordingView;
    recording_view::BandRequest m_recordingBand;
    heatmap_window::Band m_recordingDisplayBand;
    recording_view::Debounce m_recordingDebounce;
    QElapsedTimer m_recordingClock;
    QTimer* m_recordingBandTimer = nullptr;
    QTimer* m_recordingTimeout = nullptr;
    QTimer* m_recordingRetry = nullptr;
    int m_recordingNoProgress = 0;

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

Q_DECLARE_METATYPE(protocol::recordingwire::Request)
