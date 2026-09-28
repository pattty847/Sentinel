/*
Sentinel — DataProcessor
Role: Handles remote heatmap slice ingestion and forwards columns to the GPU renderer.
Inputs/Outputs: Receives heatmap slices; emits column and range reset signals for GPU upload.
Threading: Lives on a worker QThread; slots are invoked via queued connections.
Performance: Minimal processing to preserve GPU upload cadence.
Integration: Owned by UnifiedGridRenderer; participates in the GPU-only heatmap path.
Observability: Logs window/timeframe/history transitions; per-slice detail via probes (heatmap.*, footprint.*, tpo.*).
Related: DataProcessor.hpp.
Assumptions: Server is authoritative for heatmap columns.
*/
#include "DataProcessor.hpp"
#include "FootprintStreamState.hpp"
#include "TpoStreamState.hpp"
#include "VolumeProfileState.hpp"
#include "SentinelLogging.hpp"
#include "../../core/protocol/VolumeProfileSlice.hpp"
#include <algorithm>
#include <QtGlobal>
#include <QTimer>
#include <limits>
#include <cstring>
#include <bit>

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
    sLog_Data("DataProcessor symbol: " << m_activeSymbol << "->" << normalized << " (stream state cleared)");
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
        sLog_Probe("heatmap.drop", "reason=symbol symbol=" << slice.symbol
                   << " active=" << m_activeSymbol << " start=" << slice.bucketStartMs);
        return;
    }
    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_heatmapGridWidth;

    sLog_Probe("heatmap.slice", "symbol=" << slice.symbol
               << " tf=" << slice.timeframeMs
               << " start=" << slice.bucketStartMs
               << " bytes=" << slice.column.size()
               << " grid=" << resolvedWidth << "x" << slice.gridHeight
               << " price=[" << slice.minPrice << ".." << slice.maxPrice << "]"
               << " tick=" << slice.tickSize
               << " reset=" << slice.reset
               << " format=" << slice.format
               << " current=" << m_currentTimeframe_ms
               << " manual=" << m_manualTimeframeSet);

    if (m_forcedTimeframeMs > 0 && slice.timeframeMs > 0 && slice.timeframeMs != m_forcedTimeframeMs) {
        sLog_Probe("heatmap.drop", "reason=timeframe tf=" << slice.timeframeMs
                   << " forced=" << m_forcedTimeframeMs << " start=" << slice.bucketStartMs);
        return;
    }
    if (slice.timeframeMs > 0 && m_currentTimeframe_ms != slice.timeframeMs) {
        sLog_Render("Heatmap timeframe from stream: tf=" << m_currentTimeframe_ms << "->"
                    << slice.timeframeMs << " symbol=" << slice.symbol);
        m_currentTimeframe_ms = slice.timeframeMs;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }
    if (slice.column.isEmpty() || slice.timeframeMs <= 0) {
        sLog_DataN(5000, "Heatmap slice dropped: empty column or tf<=0 symbol=" << slice.symbol
                   << " tf=" << slice.timeframeMs << " bytes=" << slice.column.size()
                   << " start=" << slice.bucketStartMs);
        return;
    }

    const QString fmt = slice.format.trimmed().toLower();
    int bytesPerCell = 1;
    if (fmt == QStringLiteral("u16") || fmt == QStringLiteral("r16") ||
        fmt == QStringLiteral("r16f")) {
        bytesPerCell = 2;
    } else if (fmt != QStringLiteral("u8") && fmt != QStringLiteral("r8")) {
        // f32 and unknown formats are not supported by the ring
        sLog_DataN(5000, "Heatmap slice dropped: unsupported format=" << slice.format
                   << " symbol=" << slice.symbol << " tf=" << slice.timeframeMs);
        return;
    }
    const int rows = (slice.gridHeight > 0) ? slice.gridHeight : slice.column.size() / bytesPerCell;
    if (slice.column.size() % bytesPerCell != 0 || rows <= 0 ||
        slice.column.size() / bytesPerCell != rows) {
        sLog_DataN(5000, "Heatmap slice dropped: size mismatch bytes=" << slice.column.size()
                   << " bytesPerCell=" << bytesPerCell << " gridHeight=" << slice.gridHeight
                   << " symbol=" << slice.symbol << " tf=" << slice.timeframeMs);
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
        // Normal when the live bucket is outside a manual window (kept in cache);
        // also returned for a shape mismatch (rows, unaligned bucket, bad band).
        sLog_Probe("heatmap.drop", "reason=window-rejected start=" << slice.bucketStartMs
                   << " windowEnd=" << m_heatmapWindow.windowEndMs()
                   << " rows=" << rows << " windowRows=" << m_heatmapWindow.rows()
                   << " bytesPerCell=" << bytesPerCell);
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
    sLog_Render("Heatmap window configure: tf=" << m_heatmapWindow.timeframeMs() << "->" << timeframeMs
                << " width=" << m_heatmapWindow.width() << "->" << boundedWidth
                << " rows=" << m_heatmapWindow.rows() << "->" << rows
                << " symbol=" << m_activeSymbol);
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
    sLog_Data("Heatmap history request: symbol=" << m_activeSymbol
              << " tf=" << m_heatmapWindow.timeframeMs() << " end=" << request.endMs
              << " count=" << request.count << " gen=" << generation);
    emit heatmapHistoryFetchNeeded(m_heatmapWindow.timeframeMs(), request.endMs, request.count);
    emit heatmapHistoryStatus(true, m_heatmapWindow.oldestAvailableMs());
    QTimer::singleShot(5000, this, [this, generation]() {
        if (generation != m_heatmapFetchGeneration || !m_heatmapFetchInFlight) {
            return;
        }
        sLog_Warning("Heatmap history request timed out after 5000 ms: symbol=" << m_activeSymbol
                     << " tf=" << m_heatmapWindow.timeframeMs() << " end=" << m_heatmapFetchEndMs
                     << " count=" << m_heatmapFetchCount << " gen=" << generation);
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
    sLog_Probe("tpo.snapshot", "session=[" << snap.sessionStartMs << ".." << snap.sessionEndMs << "]"
               << " tf=" << snap.timeframeMs
               << " grid=" << snap.gridWidth << "x" << snap.gridHeight
               << " pending=" << pendingUploads.size());
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
        sLog_Data("Heatmap history ignored: symbol=" << symbol << " active=" << m_activeSymbol
                  << " tf=" << timeframeMs << " end=" << requestEndMs);
        return;
    }
    if (timeframeMs <= 0 || gridHeight <= 0) {
        sLog_Warning("Heatmap history dropped: invalid page symbol=" << symbol << " tf=" << timeframeMs
                     << " grid=" << gridWidth << "x" << gridHeight
                     << " end=" << requestEndMs << " cols=" << columns.size());
        return;
    }
    if (m_forcedTimeframeMs > 0 && timeframeMs != m_forcedTimeframeMs) {
        sLog_Data("Heatmap history ignored: tf=" << timeframeMs << " forced=" << m_forcedTimeframeMs
                  << " symbol=" << symbol << " end=" << requestEndMs);
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
    const bool applied = m_heatmapWindow.ingestHistory(page, bytesPerCell, requestEndMs, requested,
                                                       oldestAvailableMs, *update, firstPlacement);
    sLog_Data("Heatmap history received: symbol=" << symbol << " tf=" << timeframeMs
              << " end=" << requestEndMs << " cols=" << columns.size() << "/" << requested
              << " grid=" << gridWidth << "x" << gridHeight << " oldest=" << oldestAvailableMs
              << " ours=" << ours << " applied=" << applied
              << " cached=" << m_heatmapWindow.cachedColumns());
    if (applied) {
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
        sLog_Render("Heatmap server timeframe: tf=" << m_forcedTimeframeMs << "->" << timeframeMs
                    << " (window reset)");
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
        sLog_Render("Heatmap manual timeframe: tf=" << m_currentTimeframe_ms << "->" << timeframe_ms
                    << " forced=" << m_forcedTimeframeMs);
        m_currentTimeframe_ms = timeframe_ms;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }
}

bool DataProcessor::isManualTimeframeSet() const {
    return m_manualTimeframeSet;
}
