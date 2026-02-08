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
#include "GridViewState.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <QtGlobal>
#include <limits>
#include <cstring>
#include <bit>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
    m_footprintStream = std::make_unique<FootprintStreamState>();
    m_footprintStream->setGridDimensions(m_footprintGridWidth, m_footprintGridHeight);
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
    m_heatmapRangeValid = false;
    m_heatmapLastSliceStart = std::numeric_limits<int64_t>::min();
    m_heatmapHasLastColumn = false;
    m_heatmapLastColumn.clear();
    m_heatmapCache.clear();
    if (m_footprintStream) {
        m_footprintStream->clear();
    }
}

void DataProcessor::onHeatmapSliceReceived(const HeatmapSlice& slice) {
    Q_UNUSED(slice.symbol);
    Q_UNUSED(slice.midPrice);
    Q_UNUSED(slice.lastTrade);
    const int resolvedWidth = (slice.gridWidth > 0) ? slice.gridWidth : m_heatmapGridWidth;

    if (m_shuttingDown.load()) {
        return;
    }

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

    if (slice.column.isEmpty()) {
        return;
    }

    const QString fmt = slice.format.trimmed().toLower();
    int bytesPerCell = 1;
    if (fmt == QStringLiteral("u8") || fmt == QStringLiteral("r8")) {
        bytesPerCell = 1;
    } else if (fmt == QStringLiteral("u16") || fmt == QStringLiteral("r16") ||
               fmt == QStringLiteral("r16f")) {
        bytesPerCell = 2;
    } else if (fmt == QStringLiteral("f32") || fmt == QStringLiteral("r32f")) {
        bytesPerCell = 4;
    }
    const int prevHeight = m_heatmapGridHeight;
    const int prevWidth = m_heatmapGridWidth;
    int height = 0;
    QByteArray expanded;
    const int64_t lastSliceStart = m_heatmapLastSliceStart;
    bool forceReset = false;

    if (bytesPerCell <= 0 || (slice.column.size() % bytesPerCell) != 0) {
        return;
    }
    height = (slice.gridHeight > 0) ? slice.gridHeight : (slice.column.size() / bytesPerCell);
    if (height <= 0) {
        return;
    }
    if (slice.column.size() / bytesPerCell != height) {
        return;
    }
    expanded = slice.column;

    m_heatmapGridWidth = resolvedWidth;
    m_heatmapGridHeight = height;
    const double effectiveTick = slice.tickSize;
    if (lastSliceStart != std::numeric_limits<int64_t>::min()) {
        if (slice.bucketStartMs <= lastSliceStart) {
            forceReset = true;
        } else if (slice.timeframeMs > 0 && resolvedWidth > 0) {
            const int64_t maxGapMs = static_cast<int64_t>(resolvedWidth) * slice.timeframeMs;
            if (maxGapMs > 0 && (slice.bucketStartMs - lastSliceStart) > maxGapMs) {
                forceReset = true;
            }
        }
    }
    if (forceReset) {
        m_heatmapRangeValid = false;
        m_heatmapLastColumn.clear();
        m_heatmapHasLastColumn = false;
        m_heatmapCache.clear();
    }
    const bool needsReset = slice.reset || forceReset || !m_heatmapRangeValid ||
                            (prevHeight != height || prevWidth != m_heatmapGridWidth);

    if (needsReset) {
        emit heatmapRangeReset(slice.minPrice, slice.maxPrice, effectiveTick, m_heatmapGridWidth, height);
    }

    m_heatmapRangeValid = true;
    emit heatmapColumnReady(slice.bucketStartMs,
                            slice.bucketEndMs,
                            slice.timeframeMs,
                            slice.minPrice,
                            slice.maxPrice,
                            effectiveTick,
                            expanded,
                            slice.liquidityColumn,
                            slice.liquidityScale,
                            bytesPerCell);

    HeatmapGridKey key;
    key.symbol = slice.symbol.toStdString();
    key.gridWidth = m_heatmapGridWidth;
    key.gridHeight = m_heatmapGridHeight;
    key.timeframeMs = slice.timeframeMs;
    key.minPrice = slice.minPrice;
    key.maxPrice = slice.maxPrice;
    key.tickSize = effectiveTick;

    auto& cache = m_heatmapCache[key];
    const int capacity = (m_cacheCapacityOverride > 0) ? m_cacheCapacityOverride : m_heatmapGridWidth;
    if (cache.capacity != capacity) {
        cache.reset(capacity);
    }
    IGridDataSource::HeatmapHistoryColumn entry;
    entry.bucketStartMs = slice.bucketStartMs;
    entry.bucketEndMs = slice.bucketEndMs;
    entry.minPrice = slice.minPrice;
    entry.maxPrice = slice.maxPrice;
    entry.tickSize = effectiveTick;
    entry.intensity = expanded;
    entry.liquidity = slice.liquidityColumn;
    entry.liquidityScale = slice.liquidityScale;
    cache.push(std::move(entry));
    m_heatmapLastSliceStart = slice.bucketStartMs;
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
    }
}

void DataProcessor::onHeatmapHistoryReceived(const QString& symbol,
                                             int64_t timeframeMs,
                                             int gridWidth,
                                             int gridHeight,
                                             const QVector<IGridDataSource::HeatmapHistoryColumn>& columns) {
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP HISTORY RX: cols=" << columns.size()
                    << " grid=" << gridWidth << "x" << gridHeight
                    << " tf=" << timeframeMs);
    }

    if (columns.isEmpty() || gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    if (m_forcedTimeframeMs > 0 && timeframeMs > 0 && timeframeMs != m_forcedTimeframeMs) {
        return;
    }

    const auto& first = columns.front();
    if (gridHeight <= 0 || first.intensity.isEmpty()) {
        return;
    }
    if ((first.intensity.size() % gridHeight) != 0) {
        return;
    }
    const int bytesPerCellGuess = first.intensity.size() / gridHeight;
    const int bytesPerCell = (bytesPerCellGuess == 1 || bytesPerCellGuess == 2 || bytesPerCellGuess == 4)
        ? bytesPerCellGuess
        : 1;

    HeatmapGridKey key;
    key.symbol = symbol.toStdString();
    key.gridWidth = gridWidth;
    key.gridHeight = gridHeight;
    key.timeframeMs = timeframeMs;
    key.minPrice = first.minPrice;
    key.maxPrice = first.maxPrice;
    key.tickSize = first.tickSize;

    auto& cache = m_heatmapCache[key];
    const int capacity = (m_cacheCapacityOverride > 0) ? m_cacheCapacityOverride : gridWidth;
    if (cache.capacity != capacity) {
        cache.reset(capacity);
    }

    emit heatmapRangeReset(first.minPrice, first.maxPrice, first.tickSize, gridWidth, gridHeight);

    for (const auto& col : columns) {
        IGridDataSource::HeatmapHistoryColumn entry = col;
        cache.push(entry);
        emit heatmapColumnReady(col.bucketStartMs,
                                col.bucketEndMs,
                                timeframeMs,
                                col.minPrice,
                                col.maxPrice,
                                col.tickSize,
                                col.intensity,
                                col.liquidity,
                                col.liquidityScale,
                                bytesPerCell);
    }
}

size_t DataProcessor::HeatmapGridKeyHash::operator()(const HeatmapGridKey& key) const noexcept {
    auto hashCombine = [](size_t seed, size_t value) {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    size_t seed = std::hash<std::string>{}(key.symbol);
    seed = hashCombine(seed, std::hash<int>{}(key.gridWidth));
    seed = hashCombine(seed, std::hash<int>{}(key.gridHeight));
    seed = hashCombine(seed, std::hash<int64_t>{}(key.timeframeMs));
    seed = hashCombine(seed, std::hash<uint64_t>{}(std::bit_cast<uint64_t>(key.minPrice)));
    seed = hashCombine(seed, std::hash<uint64_t>{}(std::bit_cast<uint64_t>(key.maxPrice)));
    seed = hashCombine(seed, std::hash<uint64_t>{}(std::bit_cast<uint64_t>(key.tickSize)));
    return seed;
}

bool DataProcessor::HeatmapGridKeyEq::operator()(const HeatmapGridKey& a,
                                                 const HeatmapGridKey& b) const noexcept {
    return a.symbol == b.symbol &&
           a.gridWidth == b.gridWidth &&
           a.gridHeight == b.gridHeight &&
           a.timeframeMs == b.timeframeMs &&
           a.minPrice == b.minPrice &&
           a.maxPrice == b.maxPrice &&
           a.tickSize == b.tickSize;
}

void DataProcessor::HeatmapColumnCache::reset(int newCapacity) {
    capacity = newCapacity;
    writeIndex = 0;
    count = 0;
    columns.clear();
    if (capacity > 0) {
        columns.resize(static_cast<size_t>(capacity));
    }
}

void DataProcessor::HeatmapColumnCache::push(IGridDataSource::HeatmapHistoryColumn column) {
    if (capacity <= 0) {
        return;
    }
    columns[static_cast<size_t>(writeIndex)] = std::move(column);
    writeIndex = (writeIndex + 1) % capacity;
    count = std::min(count + 1, capacity);
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

void DataProcessor::setHeatmapRecenterFraction(double fraction) {
    m_heatmapRecenterFraction = std::clamp(fraction, 0.01, 0.45);
}

void DataProcessor::setCacheCapacityOverride(int capacity) {
    if (capacity > 0) {
        m_cacheCapacityOverride = capacity;
    }
}

void DataProcessor::setServerTimeframe(int64_t timeframeMs) {
    if (timeframeMs > 0) {
        m_forcedTimeframeMs = timeframeMs;
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
