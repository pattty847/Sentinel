// Dedicated QThread processor for the trade overlays (footprint, TPO, volume profile).
#pragma once
#include "TradeOverlayMapping.hpp"
#include <QObject>
#include <QElapsedTimer>
#include <atomic>
#include <memory>
#include <vector>
#include "../datasources/IGridDataSource.hpp"

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
    void onFootprintSliceReceived(const FootprintSlice& slice);
    void onTpoSliceReceived(const TpoSlice& slice);
    // Selected TPO bracket and session. Slices for any other selection (stale
    // history replies, live slices from before the server saw the selection)
    // are dropped instead of resetting the store.
    void setTpoSelection(qint64 timeframeMs, int sessionType);
    void onVolumeProfileSliceReceived(const VolumeProfileSlice& slice);

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

signals:
    void footprintColumnReady(int x, int gridWidth, int gridHeight, QByteArray columnQ16, TradeOverlayGrid grid);
    void tpoColumnReady(int x,
                        int gridWidth,
                        int gridHeight,
                        QByteArray letters,
                        int64_t sessionStartMs,
                        int64_t sessionEndMs,
                        int64_t timeframeMs, TradeOverlayGrid grid);
    void volumeProfileReady(std::vector<float> bins, VolumeProfileState::Snapshot snap);

private:
    bool m_manualTimeframeSet = false;
    QElapsedTimer m_manualTimeframeTimer;
    int64_t m_currentTimeframe_ms = 100;

    std::atomic<bool> m_shuttingDown{false};
    QString m_activeSymbol;

    int m_footprintGridWidth = 5120;
    int m_footprintGridHeight = 2048;
    std::unique_ptr<FootprintStreamState> m_footprintStream;
    int m_tpoGridWidth = 5120;
    int m_tpoGridHeight = 2048;
    uint64_t m_tpoGridGeneration = 0;
    double m_tpoMaxPrice = 0.0;
    double m_tpoTickSize = 0.0;
    std::unique_ptr<TpoStreamState> m_tpoStream;
    qint64 m_tpoSelectedTimeframeMs = 0;  // 0 = accept the slice's own selection
    int m_tpoSelectedSessionType = -1;
    std::unique_ptr<VolumeProfileState> m_vpStream;

};
