#include "HeatmapHistoryWindow.hpp"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>

namespace heatmap_history {
namespace {

using Column = IGridDataSource::HeatmapHistoryColumn;

int intensityScore(uint16_t value, int bytesPerCell) {
    const int neutral = (bytesPerCell == 1) ? 128 : 32768;
    return std::abs(static_cast<int>(value) - neutral);
}

uint16_t readIntensity(const QByteArray& bytes, int row, int bytesPerCell) {
    if (bytesPerCell == 1) {
        return static_cast<uint8_t>(bytes.at(row));
    }
    const auto* values = reinterpret_cast<const uint16_t*>(bytes.constData());
    return qFromLittleEndian(values[row]);
}

void writeIntensity(QByteArray& bytes, int row, int bytesPerCell, uint16_t value) {
    if (bytesPerCell == 1) {
        bytes[row] = static_cast<char>(value);
        return;
    }
    auto* values = reinterpret_cast<uint16_t*>(bytes.data());
    values[row] = qToLittleEndian(value);
}

} // namespace

bool buildWindow(const QVector<Column>& source,
                 int64_t timeframeMs,
                 int capacity,
                 int gridHeight,
                 int64_t requestedEndMs,
                 Window& out) {
    out = {};
    if (source.isEmpty() || timeframeMs <= 0 || capacity <= 0 || gridHeight <= 0) {
        return false;
    }

    QVector<Column> valid;
    valid.reserve(source.size());
    int bytesPerCell = 0;
    for (const auto& column : source) {
        if (column.bucketStartMs <= 0 ||
            column.intensity.isEmpty() ||
            column.intensity.size() % gridHeight != 0 ||
            !std::isfinite(column.minPrice) ||
            !std::isfinite(column.maxPrice) ||
            !std::isfinite(column.tickSize) ||
            column.maxPrice <= column.minPrice ||
            column.tickSize <= 0.0) {
            continue;
        }
        const int candidateBytes = column.intensity.size() / gridHeight;
        if (candidateBytes != 1 && candidateBytes != 2) {
            continue;
        }
        if (bytesPerCell == 0) {
            bytesPerCell = candidateBytes;
        }
        if (candidateBytes == bytesPerCell) {
            valid.push_back(column);
        }
    }
    if (valid.isEmpty() || bytesPerCell == 0) {
        return false;
    }

    std::sort(valid.begin(), valid.end(), [](const Column& a, const Column& b) {
        return a.bucketStartMs < b.bucketStartMs;
    });
    auto duplicate = std::unique(valid.begin(), valid.end(), [](const Column& a, const Column& b) {
        return a.bucketStartMs == b.bucketStartMs;
    });
    valid.erase(duplicate, valid.end());

    const int64_t lastBucket = requestedEndMs > 0
        ? requestedEndMs - (requestedEndMs % timeframeMs)
        : valid.back().bucketStartMs;
    const int64_t spanBuckets = static_cast<int64_t>(capacity - 1);
    if (spanBuckets > (std::numeric_limits<int64_t>::max() / timeframeMs)) {
        return false;
    }
    const int64_t windowSpanMs = spanBuckets * timeframeMs;
    if (lastBucket <= windowSpanMs) {
        return false;
    }
    const int64_t windowStart = lastBucket - windowSpanMs;

    double targetMin = std::numeric_limits<double>::max();
    double targetMax = std::numeric_limits<double>::lowest();
    bool haveLiquidity = false;
    for (const auto& column : valid) {
        if (column.bucketStartMs < windowStart || column.bucketStartMs > lastBucket) {
            continue;
        }
        targetMin = std::min(targetMin, column.minPrice);
        targetMax = std::max(targetMax, column.maxPrice);
        haveLiquidity = haveLiquidity ||
            column.liquidity.size() == gridHeight * static_cast<int>(sizeof(uint16_t));
    }
    // A whole requested page can fall inside a recording outage. Keep the
    // page at the requested timestamps and borrow the nearest prior price band
    // only to define its blank vertical range.
    if (!std::isfinite(targetMin) || !std::isfinite(targetMax) || targetMax <= targetMin) {
        const auto nearest = std::find_if(valid.rbegin(), valid.rend(), [lastBucket](const Column& column) {
            return column.bucketStartMs <= lastBucket;
        });
        if (nearest == valid.rend()) {
            return false;
        }
        targetMin = nearest->minPrice;
        targetMax = nearest->maxPrice;
    }
    if (!std::isfinite(targetMin) || !std::isfinite(targetMax) || targetMax <= targetMin) {
        return false;
    }
    const double targetTick = (targetMax - targetMin) / static_cast<double>(gridHeight);
    if (!std::isfinite(targetTick) || targetTick <= 0.0) {
        return false;
    }

    out.columns.resize(capacity);
    out.intensityBytesPerCell = bytesPerCell;
    out.gridHeight = gridHeight;
    out.minPrice = targetMin;
    out.maxPrice = targetMax;
    out.tickSize = targetTick;
    const int intensityBytes = gridHeight * bytesPerCell;
    const int liquidityBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
    for (int x = 0; x < capacity; ++x) {
        auto& column = out.columns[x];
        column.bucketStartMs = windowStart + static_cast<int64_t>(x) * timeframeMs;
        column.bucketEndMs = column.bucketStartMs + timeframeMs;
        column.minPrice = targetMin;
        column.maxPrice = targetMax;
        column.tickSize = targetTick;
        column.intensity = QByteArray(intensityBytes, 0);
        if (haveLiquidity) {
            column.liquidity = QByteArray(liquidityBytes, 0);
        }
        column.liquidityScale = 1.0;
    }

    for (const auto& sourceColumn : valid) {
        if (sourceColumn.bucketStartMs < windowStart || sourceColumn.bucketStartMs > lastBucket) {
            continue;
        }
        const int64_t offset = sourceColumn.bucketStartMs - windowStart;
        if (offset < 0 || offset % timeframeMs != 0) {
            continue;
        }
        const int x = static_cast<int>(offset / timeframeMs);
        if (x < 0 || x >= capacity) {
            continue;
        }

        auto& targetColumn = out.columns[x];
        const bool sourceHasLiquidity = sourceColumn.liquidity.size() == liquidityBytes;
        const auto* sourceLiquidity = sourceHasLiquidity
            ? reinterpret_cast<const uint16_t*>(sourceColumn.liquidity.constData())
            : nullptr;
        auto* targetLiquidity = haveLiquidity
            ? reinterpret_cast<uint16_t*>(targetColumn.liquidity.data())
            : nullptr;
        targetColumn.liquidityScale = sourceHasLiquidity && sourceColumn.liquidityScale > 0.0
            ? sourceColumn.liquidityScale
            : 1.0;

        for (int sourceRow = 0; sourceRow < gridHeight; ++sourceRow) {
            const double price = sourceColumn.maxPrice -
                (static_cast<double>(sourceRow) + 0.5) * sourceColumn.tickSize;
            const int targetRow = static_cast<int>(std::floor((targetMax - price) / targetTick));
            if (targetRow < 0 || targetRow >= gridHeight) {
                continue;
            }

            const uint16_t incoming = readIntensity(sourceColumn.intensity, sourceRow, bytesPerCell);
            const uint16_t existing = readIntensity(targetColumn.intensity, targetRow, bytesPerCell);
            if (incoming != 0 &&
                (existing == 0 || intensityScore(incoming, bytesPerCell) >
                                      intensityScore(existing, bytesPerCell))) {
                writeIntensity(targetColumn.intensity, targetRow, bytesPerCell, incoming);
            }

            if (sourceLiquidity && targetLiquidity) {
                const uint16_t incomingLiquidity = qFromLittleEndian(sourceLiquidity[sourceRow]);
                const uint16_t existingLiquidity = qFromLittleEndian(targetLiquidity[targetRow]);
                if (incomingLiquidity > existingLiquidity) {
                    targetLiquidity[targetRow] = qToLittleEndian(incomingLiquidity);
                }
            }
        }
    }
    return true;
}

} // namespace heatmap_history
