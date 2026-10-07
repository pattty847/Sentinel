/*
Sentinel — DataProcessor
Role: stages the trade overlays (footprint, TPO, volume profile) on their own
      grids (INV-068) and emits ready columns for the chart's overlay renderers.
Inputs/Outputs: receives overlay slices; emits footprint/TPO columns and VP bins.
Threading: Lives on a worker QThread; slots are invoked via queued connections.
Integration: Owned by UnifiedGridRenderer. The heatmap itself is the GPU layer's
      (HeatmapGpuLayer); nothing here touches it since S8a.
Observability: per-slice detail via probes (footprint.*, tpo.*).
Related: DataProcessor.hpp.
*/
#include "DataProcessor.hpp"

#include "FootprintStreamState.hpp"
#include "TpoStreamState.hpp"
#include "VolumeProfileState.hpp"
#include "SentinelLogging.hpp"
#include "../../core/protocol/VolumeProfileSlice.hpp"
#include <algorithm>
#include <QtGlobal>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<TradeOverlayGrid>();
    m_footprintStream = std::make_unique<FootprintStreamState>();
    m_footprintStream->setGridDimensions(m_footprintGridWidth, m_footprintGridHeight);
    m_tpoStream = std::make_unique<TpoStreamState>();
    m_tpoStream->reset(m_tpoGridWidth, m_tpoGridHeight);
    m_vpStream = std::make_unique<VolumeProfileState>();
}

DataProcessor::~DataProcessor() {
    stopProcessing();
}

void DataProcessor::startProcessing() {
}

void DataProcessor::stopProcessing() {
    bool expected = false;
    if (!m_shuttingDown.compare_exchange_strong(expected, true)) {
        return;
    }

    disconnect(this, nullptr, nullptr, nullptr);
    clearData();
}

void DataProcessor::clearData() {
    ++m_tpoGridGeneration;
    m_tpoMaxPrice = 0;
    m_tpoTickSize = 0;
    if (m_footprintStream) {
        m_footprintStream->clear();
    }
    if (m_tpoStream) {
        m_tpoStream->clear();
    }
    if (m_vpStream) {
        m_vpStream->clear();
    }
}

void DataProcessor::setActiveSymbol(const QString& symbol) {
    const QString normalized = symbol.trimmed().toUpper();
    if (m_activeSymbol == normalized) {
        return;
    }
    sLog_Data("DataProcessor symbol: " << m_activeSymbol << "->" << normalized << " (stream state cleared)");
    m_activeSymbol = normalized;
    clearData();
}

void DataProcessor::onFootprintSliceReceived(const FootprintSlice& slice) {
    if (!m_activeSymbol.isEmpty() && slice.symbol != m_activeSymbol) return;
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_footprintStream) {
        return;
    }
    if (slice.deltaLevelsQ16.isEmpty()) {
        return;
    }
    if (slice.format.trimmed().compare(QStringLiteral("q16_delta"), Qt::CaseInsensitive) != 0) {
        sLog_DataN(5000, "Footprint slice dropped: unsupported format=" << slice.format
                   << " symbol=" << slice.symbol << " tf=" << slice.timeframeMs);
        return;
    }

    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_footprintGridWidth;
    const int resolvedHeight = (slice.gridHeight > 0) ? slice.gridHeight : m_footprintGridHeight;
    if (resolvedWidth <= 0 || resolvedHeight <= 0) {
        sLog_DataN(5000, "Footprint slice dropped: no grid size symbol=" << slice.symbol
                   << " grid=" << resolvedWidth << "x" << resolvedHeight);
        return;
    }

    if (resolvedWidth != m_footprintGridWidth || resolvedHeight != m_footprintGridHeight) {
        sLog_Render("Footprint grid resize: " << m_footprintGridWidth << "x" << m_footprintGridHeight
                    << "->" << resolvedWidth << "x" << resolvedHeight << " symbol=" << slice.symbol);
        m_footprintGridWidth = resolvedWidth;
        m_footprintGridHeight = resolvedHeight;
        m_footprintStream->setGridDimensions(m_footprintGridWidth, m_footprintGridHeight);
    }

    const bool ok = m_footprintStream->ingestSlice(slice.bucketStartMs,
                                                   slice.bucketEndMs,
                                                   slice.timeframeMs,
                                                   m_footprintGridWidth,
                                                   m_footprintGridHeight,
                                                   slice.minPrice,
                                                   slice.maxPrice,
                                                   slice.tickSize,
                                                   slice.deltaLevelsQ16);
    if (!ok) {
        // ingestSlice rejects only malformed slices (bad times, size, price band).
        sLog_DataN(5000, "Footprint slice dropped at staging: symbol=" << slice.symbol
                   << " start=" << slice.bucketStartMs << " end=" << slice.bucketEndMs
                   << " tf=" << slice.timeframeMs
                   << " grid=" << m_footprintGridWidth << "x" << m_footprintGridHeight
                   << " bytes=" << slice.deltaLevelsQ16.size()
                   << " price=[" << slice.minPrice << ".." << slice.maxPrice << "]"
                   << " tick=" << slice.tickSize);
        return;
    }

    std::vector<FootprintStreamState::PendingUpload> pendingUploads;
    m_footprintStream->takePendingUploads(pendingUploads);
    if (pendingUploads.empty()) {
        return;
    }

    const auto snap = m_footprintStream->snapshot();
    sLog_Probe("footprint.stage", "symbol=" << slice.symbol << " start=" << slice.bucketStartMs
               << " tf=" << slice.timeframeMs << " pending=" << pendingUploads.size()
               << " grid=" << snap.gridWidth << "x" << snap.gridHeight
               << " write=" << snap.writeColumn << " filled=" << snap.filledColumns);
    for (const auto& upload : pendingUploads) {
        QByteArray columnQ16;
        if (!m_footprintStream->copyColumnForUpload(upload.x, columnQ16)) {
            sLog_DataN(1000, "Footprint upload dropped: copy failed x=" << upload.x
                       << " start=" << upload.bucketStartMs << " grid=" << snap.gridWidth
                       << "x" << snap.gridHeight);
            continue;
        }
        sLog_Probe("footprint.upload", "x=" << upload.x << " start=" << upload.bucketStartMs
                   << " bytes=" << columnQ16.size());
        const auto end = snap.lastSliceStartMs + snap.timeframeMs;
        emit footprintColumnReady(upload.x, snap.gridWidth, snap.gridHeight, std::move(columnQ16),
            {end - snap.gridWidth * snap.timeframeMs, end, snap.maxPrice, snap.tickSize, snap.resetGeneration});
    }
}

void DataProcessor::setTpoSelection(qint64 timeframeMs, int sessionType) {
    if (timeframeMs == m_tpoSelectedTimeframeMs && sessionType == m_tpoSelectedSessionType) {
        return;
    }
    sLog_Data("TPO selection: tf=" << m_tpoSelectedTimeframeMs << "->" << timeframeMs
              << " session=" << m_tpoSelectedSessionType << "->" << sessionType);
    m_tpoSelectedTimeframeMs = timeframeMs;
    m_tpoSelectedSessionType = sessionType;
}

void DataProcessor::onTpoSliceReceived(const TpoSlice& slice) {
    if (!m_activeSymbol.isEmpty() && slice.symbol != m_activeSymbol) return;
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_tpoStream || slice.letters.isEmpty()) {
        return;
    }

    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_tpoGridWidth;
    const int resolvedHeight = (slice.gridHeight > 0) ? slice.gridHeight : m_tpoGridHeight;
    if (resolvedWidth <= 0 || resolvedHeight <= 0 || slice.letters.size() != resolvedHeight ||
        slice.bucketStartMs <= 0 || slice.bucketEndMs <= slice.bucketStartMs || slice.timeframeMs <= 0) {
        sLog_DataN(5000, "TPO slice dropped: invalid shape symbol=" << slice.symbol
                   << " grid=" << resolvedWidth << "x" << resolvedHeight
                   << " letters=" << slice.letters.size()
                   << " start=" << slice.bucketStartMs << " end=" << slice.bucketEndMs
                   << " tf=" << slice.timeframeMs);
        return;
    }
    if (slice.format.trimmed().compare(QStringLiteral("tpo_ascii"), Qt::CaseInsensitive) != 0) {
        sLog_DataN(5000, "TPO slice dropped: unsupported format=" << slice.format
                   << " symbol=" << slice.symbol << " tf=" << slice.timeframeMs);
        return;
    }

    sLog_Probe("tpo.slice", "symbol=" << slice.symbol
               << " start=" << slice.bucketStartMs << " end=" << slice.bucketEndMs
               << " tf=" << slice.timeframeMs << " sessionType=" << slice.sessionType
               << " grid=" << resolvedWidth << "x" << resolvedHeight
               << " letters=" << slice.letters.size());

    if (m_tpoSelectedTimeframeMs > 0 &&
        (slice.timeframeMs != m_tpoSelectedTimeframeMs || slice.sessionType != m_tpoSelectedSessionType)) {
        sLog_Probe("tpo.slice", "dropped: selection tf=" << slice.timeframeMs << " session=" << slice.sessionType
                   << " selected tf=" << m_tpoSelectedTimeframeMs << " session=" << m_tpoSelectedSessionType);
        return;
    }
    const auto previous = m_tpoStream->snapshot();
    if (m_tpoMaxPrice != slice.maxPrice || m_tpoTickSize != slice.tickSize ||
        (previous.timeframeMs && previous.timeframeMs != slice.timeframeMs)) {
        m_tpoStream->reset(resolvedWidth, resolvedHeight);
        ++m_tpoGridGeneration;
    }
    m_tpoStream->setSessionType(slice.sessionType);

    if (resolvedWidth != m_tpoGridWidth || resolvedHeight != m_tpoGridHeight) {
        sLog_Render("TPO grid resize: " << m_tpoGridWidth << "x" << m_tpoGridHeight
                    << "->" << resolvedWidth << "x" << resolvedHeight << " symbol=" << slice.symbol);
        m_tpoGridWidth = resolvedWidth;
        m_tpoGridHeight = resolvedHeight;
        m_tpoStream->reset(m_tpoGridWidth, m_tpoGridHeight);
    }

    // Track price range for POC/VAH/VAL → price conversion downstream.
    if (slice.maxPrice > 0.0 && slice.tickSize > 0.0) {
        m_tpoMaxPrice = slice.maxPrice;
        m_tpoTickSize = slice.tickSize;
    }

    const bool ok = m_tpoStream->ingestSlice(slice.bucketStartMs,
                                             slice.bucketEndMs,
                                             slice.timeframeMs,
                                             m_tpoGridWidth,
                                             m_tpoGridHeight,
                                             slice.letters);
    if (!ok) {
        return;
    }

    std::vector<TpoStreamState::PendingUpload> pendingUploads;
    m_tpoStream->takePendingUploads(pendingUploads);
    // POC and value area are computed by the renderer at the displayed row grouping.
    for (auto& upload : pendingUploads) {
        sLog_Probe("tpo.emit", "session=[" << upload.sessionStartMs << ".." << upload.sessionEndMs << "]"
                   << " period=" << upload.x << "/" << upload.periods << " tf=" << slice.timeframeMs);
        emit tpoColumnReady(upload.x,
                            upload.periods,
                            m_tpoGridHeight,
                            std::move(upload.data),
                            upload.sessionStartMs,
                            upload.sessionEndMs,
                            slice.timeframeMs,
                            {upload.sessionStartMs, upload.sessionEndMs, m_tpoMaxPrice, m_tpoTickSize, m_tpoGridGeneration});
    }
}

void DataProcessor::onVolumeProfileSliceReceived(const VolumeProfileSlice& slice) {
    if (!m_activeSymbol.isEmpty() && slice.symbol != m_activeSymbol) return;
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_vpStream) {
        return;
    }
    if (!m_vpStream->ingestSlice(slice)) {
        // ingestSlice rejects only malformed slices.
        sLog_DataN(5000, "Volume profile slice dropped: symbol=" << slice.symbol
                   << " session=[" << slice.sessionStartMs << ".." << slice.sessionEndMs << "]"
                   << " gridHeight=" << slice.gridHeight << " tick=" << slice.tickSize
                   << " bytes=" << slice.volumeBinsF32.size());
        return;
    }
    std::vector<float> bins;
    VolumeProfileState::Snapshot snap;
    if (!m_vpStream->takePendingBins(bins, snap)) {
        return;
    }
    emit volumeProfileReady(std::move(bins), std::move(snap));
}

void DataProcessor::setPriceResolution(double resolution) {
    Q_UNUSED(resolution);
}

double DataProcessor::getPriceResolution() const {
    return 1.0;
}

void DataProcessor::addTimeframe(int timeframe_ms) {
    Q_UNUSED(timeframe_ms);
}

int64_t DataProcessor::suggestTimeframe(qint64 timeStart, qint64 timeEnd, int maxCells) const {
    Q_UNUSED(timeStart);
    Q_UNUSED(timeEnd);
    Q_UNUSED(maxCells);
    return 100;
}

int DataProcessor::getDisplayMode() const {
    return 0;
}

void DataProcessor::setTimeframe(int timeframe_ms) {
    if (timeframe_ms > 0) {
        sLog_Render("Overlay timeframe: tf=" << m_currentTimeframe_ms << "->" << timeframe_ms);
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }
}

bool DataProcessor::isManualTimeframeSet() const {
    return m_manualTimeframeSet;
}
