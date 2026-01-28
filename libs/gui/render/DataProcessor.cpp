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
#include "GridViewState.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <limits>
#include <cstring>

DataProcessor::DataProcessor(QObject* parent)
    : QObject(parent) {
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
}

void DataProcessor::onHeatmapSliceReceived(const QString& symbol,
                                           int64_t bucketStartMs,
                                           int64_t bucketEndMs,
                                           int64_t timeframeMs,
                                           double minPrice,
                                           double maxPrice,
                                           double tickSize,
                                           double midPrice,
                                           double lastTrade,
                                           const QString& format,
                                           const QByteArray& column,
                                           const QByteArray& liquidityColumn,
                                           double liquidityScale,
                                           bool reset) {
    Q_UNUSED(symbol);
    Q_UNUSED(midPrice);
    Q_UNUSED(lastTrade);

    if (m_shuttingDown.load()) {
        return;
    }

    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_Render("HEATMAP SLICE RX: tf=" << timeframeMs
                    << " rows=" << column.size()
                    << " reset=" << reset
                    << " format=" << format
                    << " current=" << m_currentTimeframe_ms
                    << " manual=" << m_manualTimeframeSet);
    }

    const QByteArray desiredTfEnv = qgetenv("SENTINEL_HEATMAP_TF");
    bool tfOk = false;
    const int64_t desiredTf = desiredTfEnv.toLongLong(&tfOk);
    if (tfOk && desiredTf > 0 && timeframeMs > 0 && timeframeMs != desiredTf) {
        return;
    }

    if (timeframeMs > 0 && m_currentTimeframe_ms != timeframeMs) {
        m_currentTimeframe_ms = timeframeMs;
        m_manualTimeframeSet = true;
        m_manualTimeframeTimer.restart();
    }

    if (column.isEmpty()) {
        return;
    }

    const QString fmt = format.trimmed().toLower();
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
    int height = 0;
    QByteArray expanded;

    if (bytesPerCell <= 0 || (column.size() % bytesPerCell) != 0) {
        return;
    }
    height = column.size() / bytesPerCell;
    if (height <= 0) {
        return;
    }
    expanded = column;

    m_heatmapGridHeight = height;
    const double effectiveTick = tickSize;
    const bool needsReset = reset || !m_heatmapRangeValid || (prevHeight != height);

    if (needsReset) {
        emit heatmapRangeReset(minPrice, maxPrice, effectiveTick, height);
    }

    m_heatmapRangeValid = true;
    emit heatmapColumnReady(bucketStartMs,
                            bucketEndMs,
                            timeframeMs,
                            minPrice,
                            maxPrice,
                            effectiveTick,
                            expanded,
                            liquidityColumn,
                            liquidityScale,
                            bytesPerCell);
}

void DataProcessor::setHeatmapGridHeight(int height) {
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
