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

heatmap_window::WallsSnapshot DataProcessor::captureHeatmapWalls(const heatmap_window::WallQuery& query) const {
    return m_heatmapWindow.captureWalls(query);
}

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<IGridDataSource::HeatmapHistoryColumn>("IGridDataSource::HeatmapHistoryColumn");
    qRegisterMetaType<QVector<IGridDataSource::HeatmapHistoryColumn>>("QVector<IGridDataSource::HeatmapHistoryColumn>");
    qRegisterMetaType<heatmap_window::UpdatePtr>("heatmap_window::UpdatePtr");
    qRegisterMetaType<protocol::recordingwire::Request>();
    qRegisterMetaType<recording::LiveView>();
    m_recordingClock.start();
    m_recordingBandTimer = new QTimer(this);
    m_recordingBandTimer->setSingleShot(true);
    m_recordingBandTimer->setTimerType(Qt::PreciseTimer);
    connect(m_recordingBandTimer, &QTimer::timeout, this, &DataProcessor::applyRecordingBand);
    m_recordingRetry = new QTimer(this);
    m_recordingRetry->setSingleShot(true);
    connect(m_recordingRetry, &QTimer::timeout, this, [this] {
        if (!m_recordingDebounce.pending) sendRecordingRequest(m_recordingEndMs, m_recordingFinalFetch);
    });
    m_recordingViewRetry = new QTimer(this);
    m_recordingViewRetry->setSingleShot(true);
    connect(m_recordingViewRetry, &QTimer::timeout, this, [this] {
        if (recordingMode() && m_recordingConnected && m_recordingBandConfirmed &&
            m_registeredView.generation == m_bandGeneration) emit recordingViewNeeded(m_registeredView);
    });
    m_recordingFinalRetry = new QTimer(this);
    m_recordingFinalRetry->setSingleShot(true);
    connect(m_recordingFinalRetry, &QTimer::timeout, this, [this] {
        if (!recordingMode() || !m_recordingConnected) return;
        if (!m_recordingInFlight && !m_recordingDebounce.pending) {
            int64_t bucket = 0;
            auto update = std::make_shared<heatmap_window::Update>();
            if (m_heatmapWindow.nextRecordingRepair(m_recordingClock.elapsed(), bucket, *update))
                publishHeatmapWindow(std::move(update), false);
            if (bucket) sendRecordingRequest(bucket, true);
        }
        if (m_heatmapWindow.unfinishedRecordingBucket()) m_recordingFinalRetry->start(2000);
    });
    m_recordingTimeout = new QTimer(this);
    m_recordingTimeout->setSingleShot(true);
    connect(m_recordingTimeout, &QTimer::timeout, this, [this] {
        if (!m_recordingInFlight) return;
        onRecordingHistoryError(m_activeSymbol, m_recordingRequestId, m_bandGeneration,
                                QStringLiteral("recording history timed out after 5000 ms"));
    });
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
    scheduleRecordingBand();
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
    if (recordingMode()) {
        // Bootstrap geometry from live metadata only; never mix legacy values into recording.
        if (!m_recordingBootstrapped && !m_recordingView.valid() &&
            slice.maxPrice > slice.minPrice && slice.tickSize > 0) {
            m_recordingBootstrapped = true;
            emit heatmapRangeReset(slice.minPrice, slice.maxPrice, slice.tickSize,
                                   m_heatmapGridWidth, recording_view::kRows);
        }
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

void DataProcessor::setHeatmapViewport(qint64 viewStartMs, qint64 viewEndMs, bool follow,
                                       double minPrice, double maxPrice, double widthPx, double heightPx) {
    if (m_shuttingDown.load()) {
        return;
    }
    // Placement only changes at bucket granularity; skip sub-bucket pans.
    const int64_t tf = std::max<int64_t>(1, m_heatmapWindow.timeframeMs());
    const HeatmapViewKey key{viewStartMs / tf, viewEndMs / tf, follow,
                             minPrice, maxPrice, widthPx, heightPx};
    if (key.startBucket == m_lastHeatmapView.startBucket &&
        key.endBucket == m_lastHeatmapView.endBucket &&
        key.follow == m_lastHeatmapView.follow &&
        (!recordingMode() ||
         (viewEndMs - viewStartMs == m_recordingView.endMs - m_recordingView.startMs &&
          key.minPrice == m_lastHeatmapView.minPrice && key.maxPrice == m_lastHeatmapView.maxPrice &&
          key.widthPx == m_lastHeatmapView.widthPx && key.heightPx == m_lastHeatmapView.heightPx))) {
        return;
    }
    m_lastHeatmapView = key;
    m_recordingView = {viewStartMs, viewEndMs, minPrice, maxPrice, widthPx, heightPx, follow};
    if (recordingMode()) scheduleRecordingBand();
    heatmap_window::Update update;
    if (m_heatmapWindow.setViewport(viewStartMs, viewEndMs, follow, update)) {
        publishHeatmapWindow(std::make_shared<heatmap_window::Update>(std::move(update)), false);
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
    resetRecordingRequest();
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
    if (recordingMode()) {
        if (m_recordingInFlight || m_recordingRetry->isActive() || m_recordingDebounce.pending || !m_recordingBand.valid()) return;
        heatmap_window::FetchRequest request;
        if (m_heatmapWindow.nextFetch(request)) sendRecordingRequest(request.endMs);
        return;
    }
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
    if (recordingMode() || m_shuttingDown.load()) {
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
        scheduleRecordingBand();
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

void DataProcessor::resetRecordingRequest() {
    ++m_bandGeneration;
    m_recordingViewRetry->stop();
    m_recordingFinalRetry->stop();
    m_recordingViewRetryMs = 1000;
    m_recordingFinalFetch = false;
    m_recordingInFlight = false;
    m_recordingBandConfirmed = false;
    m_recordingRequestId.clear();
    m_recordingBand = {};
    m_recordingNoProgress = 0;
    m_recordingRetry->stop();
    m_recordingDebounce.cancel();
    m_recordingBandTimer->stop();
    m_recordingTimeout->stop();
    m_recordingBootstrapped = false;
}

void DataProcessor::setRecordingConfig(bool requested, double minRowPx, double aspect) {
    const bool changed = m_recordingRequested != requested;
    m_recordingRequested = requested;
    m_recordingMinRowPx = std::clamp(minRowPx, 1.0, 64.0);
    m_recordingAspect = std::clamp(aspect, 0.05, 4.0);
    if (changed) resetHeatmapWindow();
    scheduleRecordingBand();
}

void DataProcessor::setRecordingCapability(bool available) {
    const bool wasRecording = recordingMode();
    m_recordingAvailable = available;
    if (wasRecording != recordingMode()) resetHeatmapWindow();
    scheduleRecordingBand();
}

void DataProcessor::setRecordingConnected(bool connected) {
    m_recordingConnected = connected;
    if (!connected) {
        m_recordingAvailable = false; // require a fresh hello on reconnect
        resetRecordingRequest();
    }
    scheduleRecordingBand();
}

void DataProcessor::refreshRecordingHistory() {
    if (!recordingMode()) return;
    if (!m_recordingBand.valid()) scheduleRecordingBand();
    else if (!m_recordingInFlight) sendRecordingRequest(0);
}

void DataProcessor::scheduleRecordingBand() {
    if (!recordingMode() || !m_recordingConnected || m_activeSymbol.isEmpty()) return;
    const auto wanted = recording_view::requestBand(m_recordingView, m_forcedTimeframeMs,
                                                   m_recordingMinRowPx, m_recordingAspect);
    if (!recording_view::needsReband(m_recordingView, m_recordingBand, wanted)) {
        m_recordingDebounce.cancel();
        m_recordingBandTimer->stop();
        return;
    }
    m_recordingDebounce.changed(m_recordingClock.elapsed());
    m_recordingBandTimer->start(recording_view::kDebounceMs);
}

void DataProcessor::applyRecordingBand() {
    if (!recordingMode() || !m_recordingConnected || !m_recordingView.valid() ||
        !m_recordingDebounce.ready(m_recordingClock.elapsed())) return;
    const auto band = recording_view::requestBand(m_recordingView, m_forcedTimeframeMs,
                                                 m_recordingMinRowPx, m_recordingAspect);
    if (!band.valid()) return;
    ensureHeatmapWindow(m_forcedTimeframeMs, m_heatmapGridWidth, recording_view::kRows);
    m_recordingBand = band;
    m_recordingNoProgress = 0;
    m_recordingRetry->stop();
    m_recordingDebounce.cancel();
    ++m_bandGeneration;
    m_recordingViewRetry->stop();
    m_recordingFinalRetry->stop();
    m_recordingViewRetryMs = 1000;
    m_recordingFinalFetch = false;
    m_recordingBandConfirmed = false;
    m_recordingInFlight = false;
    m_recordingTimeout->stop();
    auto update = std::make_shared<heatmap_window::Update>();
    // This provisional band only blanks old projections. The reply replaces it.
    const double tick = (band.maxPrice - band.minPrice) / recording_view::kRows;
    if (m_heatmapWindow.setDisplayBand({band.minPrice, band.maxPrice, tick}, m_bandGeneration, *update))
        publishHeatmapWindow(std::move(update), false);
    heatmap_window::Update placement;
    if (m_heatmapWindow.setViewport(m_recordingView.startMs, m_recordingView.endMs,
                                    m_recordingView.follow, placement))
        publishHeatmapWindow(std::make_shared<heatmap_window::Update>(std::move(placement)), false);
    sLog_Probe("heatmap.recording.reband", "symbol=" << m_activeSymbol << " tf=" << m_forcedTimeframeMs
               << " gen=" << m_bandGeneration << " lo=" << band.minPrice << " hi=" << band.maxPrice
               << " idealTick=" << band.idealTick << " rows=" << recording_view::kRows);
    sendRecordingRequest(m_recordingView.follow ? 0 : m_recordingView.endMs);
}

void DataProcessor::sendRecordingRequest(int64_t endMs, bool finalRepair) {
    if (!recordingMode() || !m_recordingConnected || m_activeSymbol.isEmpty() ||
        !m_recordingBand.valid() || m_recordingInFlight) return;
    // A final repair can supersede a pending 250ms history budget retry. Do not
    // let that stale timer send an extra repair outside the per-bucket budget.
    m_recordingRetry->stop();
    protocol::recordingwire::Request request;
    request.symbol = m_activeSymbol.toStdString();
    request.timeframeMs = m_forcedTimeframeMs;
    request.endTimeMs = endMs;
    request.rows = recording_view::kRows;
    m_recordingFinalFetch = finalRepair;
    request.count = finalRepair ? 1 : std::min(heatmap_window::ColumnWindow::kPageColumns, 2'000'000 / request.rows);
    request.priceMin = m_recordingBand.minPrice;
    request.priceMax = m_recordingBand.maxPrice;
    if (m_recordingBandConfirmed) {
        request.priceMin = m_recordingDisplayBand.minPrice;
        request.priceMax = m_recordingDisplayBand.maxPrice;
        request.displayTick = m_recordingDisplayBand.tickSize;
    }
    request.bandGeneration = m_bandGeneration;
    request.requestId = std::to_string(++m_recordingSerial);
    m_recordingRequestId = QString::fromStdString(request.requestId);
    m_heatmapWindow.setRecordingRequest(request.requestId);
    m_recordingEndMs = endMs;
    m_recordingInFlight = true;
    m_recordingTimeout->start(5000);
    sLog_Probe("heatmap.recording.request", "symbol=" << m_activeSymbol << " tf=" << request.timeframeMs
               << " gen=" << request.bandGeneration << " id=" << m_recordingRequestId
               << " end=" << endMs << " lo=" << request.priceMin << " hi=" << request.priceMax
               << " idealTick=" << m_recordingBand.idealTick << " rows=" << request.rows);
    emit recordingHistoryFetchNeeded(request);
    emit heatmapHistoryStatus(true, m_heatmapWindow.oldestAvailableMs());
}

void DataProcessor::onRecordingHistoryError(const QString& symbol, const QString& requestId,
                                           uint64_t generation, const QString& message) {
    if (symbol != m_activeSymbol || requestId != m_recordingRequestId ||
        generation != m_bandGeneration || !m_recordingInFlight) return;
    m_recordingInFlight = false;
    m_recordingTimeout->stop();
    sLog_Warning("Recording history failed: symbol=" << symbol << " gen=" << generation
                 << " id=" << requestId << " message=" << message);
    emit heatmapHistoryStatus(false, m_heatmapWindow.oldestAvailableMs());
}

void DataProcessor::onRecordingHistoryReceived(const SentinelStreamClient::RecordingHistoryPage& page) {
    if (!recordingMode() || !m_recordingInFlight || page.symbol != m_activeSymbol ||
        page.timeframeMs != m_forcedTimeframeMs || page.bandGeneration != m_bandGeneration ||
        page.requestId != m_recordingRequestId || page.requestEndMs != m_recordingEndMs) {
        sLog_Probe("heatmap.recording.stale", "symbol=" << page.symbol << " gen=" << page.bandGeneration
                   << " id=" << page.requestId << " activeGen=" << m_bandGeneration);
        return;
    }
    if (page.status != "complete" && page.status != "budget") {
        onRecordingHistoryError(page.symbol, page.requestId, page.bandGeneration, page.status + ": " + page.message);
        return;
    }
    // Discovery/index budgets can return no completed bucket yet. Retry the
    // same inclusive boundary, with a fresh id, without inventing known history.
    if (page.status == "budget" && page.scannedEndMs <= page.scannedStartMs && page.columns.empty()) {
        if (m_recordingFinalFetch) {
            onRecordingHistoryError(page.symbol, page.requestId, page.bandGeneration,
                                    "recording final repair made no progress");
            return; // the per-bucket timer owns the bounded retry budget
        }
        if (++m_recordingNoProgress > 3) {
            onRecordingHistoryError(page.symbol, page.requestId, page.bandGeneration,
                                    "recording budget made no progress after three retries");
            return;
        }
        m_recordingInFlight = false;
        m_recordingTimeout->stop();
        m_recordingRetry->start(250);
        return;
    }
    m_recordingNoProgress = 0;
    const heatmap_window::Band band{page.bandLo, page.bandLo + page.bandRows * page.bandTick, page.bandTick};
    if (page.bandRows != recording_view::kRows || !band.valid() ||
        !std::isfinite(band.minPrice) || !std::isfinite(band.maxPrice) || !std::isfinite(band.tickSize) ||
        (m_recordingBandConfirmed && !band.sameAs(m_recordingDisplayBand)) ||
        page.valueEncoding != "absolute_log_size" || !(page.sizeFloor > 0) ||
        !std::isfinite(page.sizeFloor) || !(page.codesPerOctave > 0) || !std::isfinite(page.codesPerOctave)) {
        onRecordingHistoryError(page.symbol, page.requestId, page.bandGeneration, "invalid recording band/scale");
        return;
    }
    for (const auto& c : page.columns) {
        const heatmap_window::Band columnBand{c.minPrice, c.maxPrice, c.tickSize};
        if (!columnBand.sameAs(band) || c.bucketStartMs <= 0 || c.bucketStartMs % page.timeframeMs != 0 ||
            c.intensity.size() != page.bandRows * 2 || c.liquidity.size() != page.bandRows * 2 ||
            c.validity.size() != (page.bandRows + 7) / 8 || c.liquidityScale < 0 ||
            !std::isfinite(c.liquidityScale)) {
            onRecordingHistoryError(page.symbol, page.requestId, page.bandGeneration, "invalid recording column");
            return;
        }
    }
    m_recordingInFlight = false;
    m_recordingTimeout->stop();
    auto update = std::make_shared<heatmap_window::Update>();
    if (m_heatmapWindow.setDisplayBand(band, m_bandGeneration, *update))
        publishHeatmapWindow(std::move(update), false);
    const bool registerView = !m_recordingBandConfirmed;
    m_recordingBandConfirmed = true;
    m_recordingDisplayBand = band;
    if (registerView) {
        m_registeredView = {page.symbol.toStdString(), page.layer.toStdString(), page.timeframeMs,
            {page.bandLo, page.bandTick, static_cast<uint32_t>(page.bandRows)}, page.bandGeneration};
        emit recordingViewNeeded(m_registeredView);
    }
    std::vector<heatmap_window::Column> columns;
    columns.reserve(page.columns.size());
    for (const auto& c : page.columns) {
        columns.push_back({c.bucketStartMs, c.minPrice, c.maxPrice, c.tickSize,
                           c.intensity, c.liquidity, c.liquidityScale, c.validity,
                           c.observedMs, (c.flags & recording::kProvisional) != 0});
    }
    update = std::make_shared<heatmap_window::Update>();
    bool first = false;
    if (m_heatmapWindow.ingestRecording(columns, page.bandGeneration, page.requestId.toStdString(),
        page.scannedStartMs, page.scannedEndMs, page.exhausted, page.oldestAvailableMs,
        page.latestAvailableMs, page.sizeFloor, page.codesPerOctave, *update, first))
        publishHeatmapWindow(std::move(update), first);
    sLog_Probe("heatmap.recording.page", "symbol=" << page.symbol << " gen=" << page.bandGeneration
               << " id=" << page.requestId << " tick=" << page.bandTick << " lo=" << page.bandLo
               << " columns=" << page.columns.size() << " scanned=[" << page.scannedStartMs
               << ".." << page.scannedEndMs << ") next=" << page.nextEndMs << " exhausted=" << page.exhausted);
    emit heatmapHistoryStatus(false, m_heatmapWindow.oldestAvailableMs());
    if (m_recordingFinalFetch) {
        m_recordingFinalFetch = false;
        return; // a targeted final repair must not initiate another history walk
    }
    // Walk only as far as the visible window and its prefetch margin. A short
    // budget page carries an explicit continuation, never an inferred floor.
    heatmap_window::FetchRequest missing;
    if (!m_recordingDebounce.pending && m_heatmapWindow.nextFetch(missing)) {
        if (!page.exhausted && page.nextEndMs > 0 &&
            (page.requestEndMs == 0 || page.nextEndMs < page.requestEndMs))
            sendRecordingRequest(std::max(missing.endMs, page.nextEndMs));
        else if (page.scannedEndMs > page.scannedStartMs)
            sendRecordingRequest(missing.endMs);
    }
}

void DataProcessor::onRecordingLiveReceived(const SentinelStreamClient::RecordingHistoryPage& page) {
    if (!recordingMode() || !m_recordingConnected || !m_recordingBandConfirmed ||
        page.symbol != m_activeSymbol || page.bandGeneration != m_bandGeneration ||
        page.timeframeMs != m_forcedTimeframeMs || page.status != "complete" ||
        page.valueEncoding != "absolute_log_size" || page.bandRows != recording_view::kRows ||
        page.columns.size() > 2) return;
    const heatmap_window::Band band{page.bandLo, page.bandLo + page.bandRows * page.bandTick, page.bandTick};
    if (!band.sameAs(m_recordingDisplayBand)) return;
    std::vector<heatmap_window::Column> columns;
    columns.reserve(page.columns.size());
    for (const auto& c : page.columns) {
        if (!heatmap_window::Band{c.minPrice, c.maxPrice, c.tickSize}.sameAs(band) ||
            c.bucketStartMs <= 0 || c.bucketStartMs % page.timeframeMs ||
            c.intensity.size() != page.bandRows * 2 || c.liquidity.size() != page.bandRows * 2 ||
            c.validity.size() != (page.bandRows + 7) / 8 || c.liquidityScale < 0 ||
            !std::isfinite(c.liquidityScale)) return;
        columns.push_back({c.bucketStartMs, c.minPrice, c.maxPrice, c.tickSize,
            c.intensity, c.liquidity, c.liquidityScale, c.validity,
                           c.observedMs, (c.flags & recording::kProvisional) != 0});
    }
    auto update = std::make_shared<heatmap_window::Update>();
    bool first = false;
    if (m_heatmapWindow.ingestRecording(columns, page.bandGeneration, {}, 0, 0, false, 0,
        0, page.sizeFloor, page.codesPerOctave, *update, first, true))
        publishHeatmapWindow(std::move(update), first);
    m_recordingViewRetry->stop();
    m_recordingViewRetryMs = 1000;
    if (m_heatmapWindow.unfinishedRecordingBucket() && !m_recordingFinalRetry->isActive())
        m_recordingFinalRetry->start(2000);
    sLog_Probe("heatmap.recording.live", "symbol=" << page.symbol << " gen=" << page.bandGeneration
        << " columns=" << columns.size() << " bucket=" << (columns.empty() ? 0 : columns.back().bucketStartMs));
}

void DataProcessor::onRecordingViewError(const QString& symbol, uint64_t generation, const QString& code,
                                        const QString& message, int retryMs) {
    if (!recordingMode() || !m_recordingConnected || !m_recordingBandConfirmed ||
        symbol != m_activeSymbol || generation != m_bandGeneration) return;
    sLog_Warning("Recording view rejected: symbol=" << symbol << " gen=" << generation
        << " code=" << code << " message=" << message);
    if (m_recordingViewRetry->isActive()) return;
    const int delay = std::clamp(std::max(retryMs, m_recordingViewRetryMs), 1000, 30000);
    m_recordingViewRetry->start(delay);
    m_recordingViewRetryMs = std::min(30000, delay * 2);
}
