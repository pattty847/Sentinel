/*
Sentinel — DataProcessor
Role: Handles remote heatmap slice ingestion and forwards columns to the GPU renderer.
Inputs/Outputs: Receives heatmap slices; emits column and range reset signals for GPU upload.
Threading: Lives on a worker QThread; slots are invoked via queued connections.
Performance: Minimal processing to preserve GPU upload cadence.
Integration: Owned by UnifiedGridRenderer; participates in the GPU-only heatmap path.
Observability: Logs heatmap ingest when debug flags are enabled.
Related: DataProcessor.hpp.
Assumptions: Server is authoritative for heatmap columns.
*/
#include "DataProcessor.hpp"
#include "FootprintStreamState.hpp"
#include "TpoStreamState.hpp"
#include "VolumeProfileState.hpp"
#include "TpoDebugTrace.hpp"
#include "SentinelLogging.hpp"
#include "../../core/protocol/VolumeProfileSlice.hpp"
#include <algorithm>
#include <QtGlobal>
#include <QTimer>
#include <limits>
#include <cstring>
#include <bit>
#include <sstream>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<IGridDataSource::HeatmapHistoryColumn>("IGridDataSource::HeatmapHistoryColumn");
    qRegisterMetaType<QVector<IGridDataSource::HeatmapHistoryColumn>>("QVector<IGridDataSource::HeatmapHistoryColumn>");
    qRegisterMetaType<heatmap_window::UpdatePtr>("heatmap_window::UpdatePtr");
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
    resetHeatmapWindow();
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
    m_activeSymbol = normalized;
    clearData();
}

void DataProcessor::onHeatmapSliceReceived(const HeatmapSlice& slice) {
    Q_UNUSED(slice.midPrice);
    Q_UNUSED(slice.lastTrade);
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_activeSymbol.isEmpty() && slice.symbol != m_activeSymbol) {
        return;
    }
    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_heatmapGridWidth;

    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP SLICE RX: tf=" << slice.timeframeMs
                    << " rows=" << slice.column.size()
                    << " grid=" << resolvedWidth << "x" << slice.gridHeight
                    << " reset=" << slice.reset
                    << " format=" << slice.format
                    << " current=" << m_currentTimeframe_ms
                    << " manual=" << m_manualTimeframeSet);
    }

    if (m_forcedTimeframeMs > 0 && slice.timeframeMs > 0 && slice.timeframeMs != m_forcedTimeframeMs) {
        return;
    }
    if (slice.timeframeMs > 0 && m_currentTimeframe_ms != slice.timeframeMs) {
        m_currentTimeframe_ms = slice.timeframeMs;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }
    if (slice.column.isEmpty() || slice.timeframeMs <= 0) {
        return;
    }

    const QString fmt = slice.format.trimmed().toLower();
    int bytesPerCell = 1;
    if (fmt == QStringLiteral("u16") || fmt == QStringLiteral("r16") ||
        fmt == QStringLiteral("r16f")) {
        bytesPerCell = 2;
    } else if (fmt != QStringLiteral("u8") && fmt != QStringLiteral("r8")) {
        return;  // f32 and unknown formats are not supported by the ring
    }
    if (slice.column.size() % bytesPerCell != 0) {
        return;
    }
    const int rows = (slice.gridHeight > 0) ? slice.gridHeight : slice.column.size() / bytesPerCell;
    if (rows <= 0 || slice.column.size() / bytesPerCell != rows) {
        return;
    }

    ensureHeatmapWindow(slice.timeframeMs, resolvedWidth, rows);
    heatmap_window::Column column;
    column.bucketStartMs = slice.bucketStartMs;
    column.minPrice = slice.minPrice;
    column.maxPrice = slice.maxPrice;
    column.tickSize = slice.tickSize;
    column.intensity = slice.column;
    column.liquidity = slice.liquidityColumn;
    column.liquidityScale = slice.liquidityScale;

    auto update = std::make_shared<heatmap_window::Update>();
    bool firstPlacement = false;
    if (!m_heatmapWindow.ingestLive(column, bytesPerCell, *update, firstPlacement)) {
        return;
    }
    publishHeatmapWindow(std::move(update), firstPlacement);
    if (firstPlacement) {
        requestHeatmapFetch();
    }
}

void DataProcessor::setHeatmapViewport(qint64 viewStartMs, qint64 viewEndMs, bool follow) {
    if (m_shuttingDown.load()) {
        return;
    }
    // Placement only changes at bucket granularity; skip sub-bucket pans.
    const int64_t tf = std::max<int64_t>(1, m_heatmapWindow.timeframeMs());
    const HeatmapViewKey key{viewStartMs / tf, viewEndMs / tf, follow};
    if (key.startBucket == m_lastHeatmapView.startBucket &&
        key.endBucket == m_lastHeatmapView.endBucket &&
        key.follow == m_lastHeatmapView.follow) {
        return;
    }
    m_lastHeatmapView = key;
    auto update = std::make_shared<heatmap_window::Update>();
    if (m_heatmapWindow.setViewport(viewStartMs, viewEndMs, follow, *update)) {
        publishHeatmapWindow(std::move(update), false);
    }
    requestHeatmapFetch();
}

void DataProcessor::ensureHeatmapWindow(int64_t timeframeMs, int width, int rows) {
    const int boundedWidth = std::clamp(width, heatmap_window::ColumnWindow::kPageColumns, 16384);
    if (m_heatmapWindow.timeframeMs() == timeframeMs &&
        m_heatmapWindow.width() == boundedWidth &&
        (m_heatmapWindow.rows() == 0 || m_heatmapWindow.rows() == rows)) {
        return;
    }
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP WINDOW CONFIGURE: tf=" << m_heatmapWindow.timeframeMs() << "->" << timeframeMs
                    << " width=" << m_heatmapWindow.width() << "->" << boundedWidth
                    << " rows=" << m_heatmapWindow.rows() << "->" << rows);
    }
    m_heatmapWindow.configure(timeframeMs, boundedWidth,
                              boundedWidth + 4 * heatmap_window::ColumnWindow::kPageColumns);
    m_heatmapGridWidth = boundedWidth;
    m_heatmapGridHeight = rows;
    m_heatmapFetchInFlight = false;
    ++m_heatmapFetchGeneration;
    m_lastHeatmapView = {};
}

void DataProcessor::resetHeatmapWindow() {
    if (m_heatmapWindow.timeframeMs() > 0) {
        m_heatmapWindow.configure(m_heatmapWindow.timeframeMs(), m_heatmapWindow.width(),
                                  m_heatmapWindow.width() + 4 * heatmap_window::ColumnWindow::kPageColumns);
    }
    m_heatmapFetchInFlight = false;
    ++m_heatmapFetchGeneration;
    m_lastHeatmapView = {};
}

void DataProcessor::publishHeatmapWindow(std::shared_ptr<heatmap_window::Update> update,
                                         bool firstPlacement) {
    if (firstPlacement) {
        // Placeholder viewport and axis init for a fresh window (FM-034).
        emit heatmapRangeReset(update->band.minPrice, update->band.maxPrice, update->band.tickSize,
                               update->width, update->rows);
    }
    emit heatmapWindowUpdated(std::move(update));
}

void DataProcessor::requestHeatmapFetch() {
    if (m_heatmapFetchInFlight) {
        return;
    }
    heatmap_window::FetchRequest request;
    if (!m_heatmapWindow.nextFetch(request)) {
        return;
    }
    m_heatmapFetchInFlight = true;
    m_heatmapFetchEndMs = request.endMs;
    m_heatmapFetchCount = request.count;
    const uint64_t generation = ++m_heatmapFetchGeneration;
    emit heatmapHistoryFetchNeeded(m_heatmapWindow.timeframeMs(), request.endMs, request.count);
    emit heatmapHistoryStatus(true, m_heatmapWindow.oldestAvailableMs());
    QTimer::singleShot(5000, this, [this, generation]() {
        if (generation != m_heatmapFetchGeneration || !m_heatmapFetchInFlight) {
            return;
        }
        m_heatmapFetchInFlight = false;
        emit heatmapHistoryStatus(false, m_heatmapWindow.oldestAvailableMs());
    });
}

void DataProcessor::onFootprintSliceReceived(const FootprintSlice& slice) {
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
        return;
    }

    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_footprintGridWidth;
    const int resolvedHeight = (slice.gridHeight > 0) ? slice.gridHeight : m_footprintGridHeight;
    if (resolvedWidth <= 0 || resolvedHeight <= 0) {
        return;
    }

    if (resolvedWidth != m_footprintGridWidth || resolvedHeight != m_footprintGridHeight) {
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
    if (!ok && qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Footprint slice dropped at staging: symbol=%1 start=%2 end=%3 tfMs=%4 grid=%5x%6 bytes=%7")
                       .arg(slice.symbol)
                       .arg(slice.bucketStartMs)
                       .arg(slice.bucketEndMs)
                       .arg(slice.timeframeMs)
                       .arg(m_footprintGridWidth)
                       .arg(m_footprintGridHeight)
                       .arg(slice.deltaLevelsQ16.size()));
        return;
    }
    if (!ok) {
        return;
    }

    std::vector<FootprintStreamState::PendingUpload> pendingUploads;
    m_footprintStream->takePendingUploads(pendingUploads);
    if (pendingUploads.empty()) {
        return;
    }

    const auto snap = m_footprintStream->snapshot();
    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        sLog_Debug(QString("Footprint staged: pending=%1 grid=%2x%3 write=%4 filled=%5")
                       .arg(static_cast<int>(pendingUploads.size()))
                       .arg(snap.gridWidth)
                       .arg(snap.gridHeight)
                       .arg(snap.writeColumn)
                       .arg(snap.filledColumns));
    }
    for (const auto& upload : pendingUploads) {
        QByteArray columnQ16;
        if (!m_footprintStream->copyColumnForUpload(upload.x, columnQ16)) {
            continue;
        }
        if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
            sLog_Debug(QString("Footprint upload ready: x=%1 bytes=%2")
                           .arg(upload.x)
                           .arg(columnQ16.size()));
        }
        emit footprintColumnReady(upload.x, snap.gridWidth, snap.gridHeight, std::move(columnQ16));
    }
}

void DataProcessor::onTpoSliceReceived(const TpoSlice& slice) {
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_tpoStream || slice.letters.isEmpty()) {
        return;
    }

    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_tpoGridWidth;
    const int resolvedHeight = (slice.gridHeight > 0) ? slice.gridHeight : m_tpoGridHeight;
    if (resolvedWidth <= 0 || resolvedHeight <= 0 || slice.letters.size() != resolvedHeight) {
        return;
    }
    if (slice.bucketStartMs <= 0 || slice.bucketEndMs <= slice.bucketStartMs || slice.timeframeMs <= 0) {
        return;
    }
    if (slice.format.trimmed().compare(QStringLiteral("tpo_ascii"), Qt::CaseInsensitive) != 0) {
        return;
    }

    if (tpo_debug::enabled()) {
        std::ostringstream payload;
        payload << "{"
                << "\"symbol\":\"" << slice.symbol.toStdString() << "\""
                << ",\"bucketStartMs\":" << slice.bucketStartMs
                << ",\"bucketEndMs\":" << slice.bucketEndMs
                << ",\"timeframeMs\":" << slice.timeframeMs
                << ",\"sessionType\":" << slice.sessionType
                << ",\"gridWidth\":" << resolvedWidth
                << ",\"gridHeight\":" << resolvedHeight
                << ",\"lettersBytes\":" << slice.letters.size()
                << "}";
        tpo_debug::append("DataProcessor.cpp:onTpoSliceReceived",
                          "tpo_slice_ingest",
                          "H1",
                          payload.str());
    }

    m_tpoStream->setSessionType(slice.sessionType);

    if (resolvedWidth != m_tpoGridWidth || resolvedHeight != m_tpoGridHeight) {
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

    // Emit POC/VAH/VAL after each successful ingest.
    if (m_tpoMaxPrice > 0.0 && m_tpoTickSize > 0.0) {
        const auto pvv = m_tpoStream->computePocVahVal();
        if (pvv.valid) {
            emit tpoPocVahValReady(pvv.pocRow, pvv.vahRow, pvv.valRow,
                                   m_tpoGridHeight,
                                   m_tpoMaxPrice, m_tpoTickSize);
        }
    }

    std::vector<TpoStreamState::PendingUpload> pendingUploads;
    m_tpoStream->takePendingUploads(pendingUploads);
    if (pendingUploads.empty()) {
        return;
    }

    const auto snap = m_tpoStream->snapshot();
    if (tpo_debug::enabled()) {
        std::ostringstream payload;
        payload << "{"
                << "\"sessionStartMs\":" << snap.sessionStartMs
                << ",\"sessionEndMs\":" << snap.sessionEndMs
                << ",\"timeframeMs\":" << snap.timeframeMs
                << ",\"gridWidth\":" << snap.gridWidth
                << ",\"gridHeight\":" << snap.gridHeight
                << ",\"pendingUploads\":" << pendingUploads.size()
                << "}";
        tpo_debug::append("DataProcessor.cpp:onTpoSliceReceived",
                          "tpo_snapshot_after_ingest",
                          "H2",
                          payload.str());
    }
    for (auto& upload : pendingUploads) {
        emit tpoColumnReady(upload.x,
                            snap.gridWidth,
                            snap.gridHeight,
                            std::move(upload.data),
                            snap.sessionStartMs,
                            snap.sessionEndMs,
                            snap.timeframeMs);
    }
}

void DataProcessor::onHeatmapHistoryReceived(const QString& symbol,
                                             int64_t timeframeMs,
                                             int gridWidth,
                                             int gridHeight,
                                             int64_t requestEndMs,
                                             int64_t oldestAvailableMs,
                                             const QVector<IGridDataSource::HeatmapHistoryColumn>& columns) {
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_activeSymbol.isEmpty() && symbol != m_activeSymbol) {
        return;
    }
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP HISTORY RX: cols=" << columns.size()
                    << " grid=" << gridWidth << "x" << gridHeight
                    << " tf=" << timeframeMs
                    << " end=" << requestEndMs);
    }
    if (timeframeMs <= 0 || gridHeight <= 0 ||
        (m_forcedTimeframeMs > 0 && timeframeMs != m_forcedTimeframeMs)) {
        return;
    }

    // A page reports its own size as grid_width; the window width comes from the
    // live stream and server config only.
    const int windowWidth = m_heatmapWindow.width() > 0 ? m_heatmapWindow.width()
                                                        : std::max(gridWidth, m_heatmapGridWidth);
    ensureHeatmapWindow(timeframeMs, windowWidth, gridHeight);
    std::vector<heatmap_window::Column> page;
    page.reserve(static_cast<size_t>(columns.size()));
    int bytesPerCell = 2;
    for (const auto& source : columns) {
        heatmap_window::Column column;
        column.bucketStartMs = source.bucketStartMs;
        column.minPrice = source.minPrice;
        column.maxPrice = source.maxPrice;
        column.tickSize = source.tickSize;
        column.intensity = source.intensity;
        column.liquidity = source.liquidity;
        column.liquidityScale = source.liquidityScale;
        bytesPerCell = std::max(1, static_cast<int>(source.intensity.size()) / gridHeight);
        page.push_back(std::move(column));
    }

    const bool ours = m_heatmapFetchInFlight && requestEndMs == m_heatmapFetchEndMs;
    const int requested = ours ? m_heatmapFetchCount : heatmap_window::ColumnWindow::kPageColumns;
    if (ours) {
        m_heatmapFetchInFlight = false;
    }

    auto update = std::make_shared<heatmap_window::Update>();
    bool firstPlacement = false;
    if (m_heatmapWindow.ingestHistory(page, bytesPerCell, requestEndMs, requested,
                                      oldestAvailableMs, *update, firstPlacement)) {
        publishHeatmapWindow(std::move(update), firstPlacement);
    }
    emit heatmapHistoryStatus(m_heatmapFetchInFlight, m_heatmapWindow.oldestAvailableMs());
    requestHeatmapFetch();
}

void DataProcessor::onVolumeProfileSliceReceived(const VolumeProfileSlice& slice) {
    if (m_shuttingDown.load()) {
        return;
    }
    if (!m_vpStream) {
        return;
    }
    if (!m_vpStream->ingestSlice(slice)) {
        return;
    }
    std::vector<float> bins;
    VolumeProfileState::Snapshot snap;
    if (!m_vpStream->takePendingBins(bins, snap)) {
        return;
    }
    emit volumeProfileReady(std::move(bins), std::move(snap));
}

void DataProcessor::setHeatmapGridHeight(int height) {
    if (height > 0) {
        m_heatmapGridHeight = height;
    }
}

void DataProcessor::setHeatmapGridDimensions(int width, int height) {
    if (width > 0) {
        m_heatmapGridWidth = width;
    }
    if (height > 0) {
        m_heatmapGridHeight = height;
    }
}

void DataProcessor::setHeatmapIntensityScale(double scale) {
    if (scale > 0.0) {
        m_heatmapIntensityScale = scale;
    }
}

void DataProcessor::setServerTimeframe(int64_t timeframeMs) {
    if (timeframeMs > 0 && timeframeMs != m_forcedTimeframeMs) {
        m_forcedTimeframeMs = timeframeMs;
        resetHeatmapWindow();
    }
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
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
        
        sLog_Render("MANUAL TIMEFRAME SET: " << timeframe_ms << "ms");
    }
}

bool DataProcessor::isManualTimeframeSet() const {
    return m_manualTimeframeSet;
}
