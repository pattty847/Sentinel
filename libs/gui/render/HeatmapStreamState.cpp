/*
Sentinel — HeatmapStreamState
Owns ring cursor, pending uploads, and time alignment state for GPU heatmap.
*/
#include "HeatmapStreamState.hpp"

#include <QtEndian>
#include <algorithm>
#include <limits>

void HeatmapStreamState::reset(int gridWidth, int gridHeight, double minPrice, double maxPrice, double tickSize) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    resetLocked(gridWidth, gridHeight);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
}

void HeatmapStreamState::setGridDimensions(int gridWidth, int gridHeight) {
    if (gridWidth <= 0 || gridHeight <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_gridWidth == gridWidth && m_gridHeight == gridHeight) {
        return;
    }
    resetLocked(gridWidth, gridHeight);
}

void HeatmapStreamState::setAppendMs(int appendMs) {
    if (appendMs <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_appendMs = appendMs;
}

void HeatmapStreamState::updateRange(double minPrice, double maxPrice, double tickSize) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_minPrice = minPrice;
    m_maxPrice = maxPrice;
    m_tickSize = tickSize;
}

bool HeatmapStreamState::applyWindow(const WindowPlacement& placement,
                                     std::vector<SlotColumn>&& slotColumns,
                                     qint64 nowMs) {
    const int gridWidth = placement.gridWidth;
    const int gridHeight = placement.gridHeight;
    const int bytesPerCell = placement.bytesPerCell;
    if (placement.timeframeMs <= 0 || gridWidth <= 0 || gridHeight <= 0 ||
        (bytesPerCell != 1 && bytesPerCell != 2) ||
        placement.newestSlot < 0 || placement.newestSlot >= gridWidth) {
        return false;
    }
    const int intensityBytes = gridHeight * bytesPerCell;
    const int liquidityBytes = gridHeight * static_cast<int>(sizeof(uint16_t));
    for (const auto& slot : slotColumns) {
        if (slot.x < 0 || slot.x >= gridWidth || slot.intensity.size() != intensityBytes) {
            return false;
        }
    }

    // Lock order matches resetLocked: state, then upload, label and ring.
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const bool reshape = m_gridWidth != gridWidth || m_gridHeight != gridHeight ||
                         m_intensityBytesPerCell != bytesPerCell;
    if (reshape) {
        resetLocked(gridWidth, gridHeight);
        m_intensityBytesPerCell = bytesPerCell;
    }
    const bool replaceAll = placement.full || reshape;

    m_appendMs = static_cast<int>(placement.timeframeMs);
    m_minPrice = placement.minPrice;
    m_maxPrice = placement.maxPrice;
    m_tickSize = placement.tickSize;
    m_writeColumn = placement.newestSlot;
    m_filledColumns = gridWidth;
    m_lastSliceStartMs = placement.windowEndMs;
    m_timeOriginMs = placement.windowEndMs -
        static_cast<int64_t>(gridWidth - 1) * placement.timeframeMs;
    const int64_t desiredBase = placement.windowEndMs - nowMs;
    if (m_streamBaseMs == std::numeric_limits<int64_t>::min()) {
        m_streamBaseMs = desiredBase;
    } else if (placement.liveEdge &&
               std::llabs(desiredBase - m_streamBaseMs) >
                   std::max<int64_t>(1, placement.timeframeMs / 2)) {
        m_streamBaseMs = desiredBase;
    }
    m_lastAppendMs = nowMs;

    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        if (replaceAll) {
            m_pendingUploads.clear();
            m_pendingUploads.reserve(slotColumns.size());
        }
        for (const auto& slot : slotColumns) {
            auto it = replaceAll ? m_pendingUploads.end()
                                 : std::find_if(m_pendingUploads.begin(), m_pendingUploads.end(),
                                                [&slot](const PendingColumn& p) { return p.x == slot.x; });
            if (it == m_pendingUploads.end()) {
                m_pendingUploads.push_back({slot.x, slot.intensity, slot.liquidity, slot.liquidityScale});
            } else {
                *it = {slot.x, slot.intensity, slot.liquidity, slot.liquidityScale};
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_labelUploadMutex);
        if (replaceAll) {
            m_pendingLabelUploads.clear();
            m_pendingLabelUploads.reserve(slotColumns.size());
        }
        for (const auto& slot : slotColumns) {
            PendingLabelColumn label;
            label.x = slot.x;
            label.intensity = slot.intensity;
            label.liquidity = slot.liquidity;
            label.liquidityScale = slot.liquidityScale;
            label.haveLiquidity = slot.liquidity.size() == liquidityBytes;
            auto it = replaceAll ? m_pendingLabelUploads.end()
                                 : std::find_if(m_pendingLabelUploads.begin(), m_pendingLabelUploads.end(),
                                                [&slot](const PendingLabelColumn& p) { return p.x == slot.x; });
            if (it == m_pendingLabelUploads.end()) {
                m_pendingLabelUploads.push_back(std::move(label));
            } else {
                *it = std::move(label);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const size_t ringSize = static_cast<size_t>(gridWidth) * gridHeight;
        if (m_intensityRing.size() != ringSize || replaceAll) {
            m_intensityRing.assign(ringSize, 0);
            m_liquidityRing.assign(ringSize, 0);
            m_liquidityScales.assign(static_cast<size_t>(gridWidth), 1.0);
            m_liquidityAvailable = false;
        }
        for (const auto& slot : slotColumns) {
            const bool haveLiquidity = slot.liquidity.size() == liquidityBytes;
            for (int y = 0; y < gridHeight; ++y) {
                const size_t index = static_cast<size_t>(y) * gridWidth + slot.x;
                if (bytesPerCell == 1) {
                    m_intensityRing[index] =
                        static_cast<uint16_t>(static_cast<uint8_t>(slot.intensity.at(y))) * 257;
                } else {
                    m_intensityRing[index] = qFromLittleEndian(
                        reinterpret_cast<const uint16_t*>(slot.intensity.constData())[y]);
                }
                m_liquidityRing[index] = haveLiquidity
                    ? qFromLittleEndian(reinterpret_cast<const uint16_t*>(slot.liquidity.constData())[y])
                    : 0;
            }
            m_liquidityScales[static_cast<size_t>(slot.x)] = slot.liquidityScale > 0.0 ? slot.liquidityScale : 1.0;
            m_liquidityAvailable = m_liquidityAvailable || haveLiquidity;
        }
    }
    return true;
}

void HeatmapStreamState::updateTimeOffset(float fractionalOffset) {
    int gridWidth = 0;
    int writeColumn = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        gridWidth = m_gridWidth;
        writeColumn = m_writeColumn;
    }
    if (gridWidth <= 0) {
        return;
    }
    const int oldestColumn = (writeColumn + 1) % gridWidth;
    const float offset = (static_cast<float>(oldestColumn) + fractionalOffset) /
                         static_cast<float>(gridWidth);
    m_timeOffset.store(offset);
}

void HeatmapStreamState::setIntensityBytesPerCell(int bytesPerCell) {
    if (bytesPerCell != 1 && bytesPerCell != 2) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_intensityBytesPerCell = bytesPerCell;
}

int HeatmapStreamState::intensityBytesPerCell() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_intensityBytesPerCell;
}

HeatmapStreamState::Snapshot HeatmapStreamState::snapshot() const {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snap.gridWidth = m_gridWidth;
        snap.gridHeight = m_gridHeight;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
        snap.filledColumns = m_filledColumns;
        snap.minPrice = m_minPrice;
        snap.maxPrice = m_maxPrice;
        snap.tickSize = m_tickSize;
    }
    snap.timeOffset = m_timeOffset.load();
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        snap.liquidityAvailable = m_liquidityAvailable;
    }
    return snap;
}

int HeatmapStreamState::writeColumn() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_writeColumn;
}

qint64 HeatmapStreamState::lastAppendMs() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastAppendMs;
}

int HeatmapStreamState::pendingUploadCount() const {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    return static_cast<int>(m_pendingUploads.size());
}

bool HeatmapStreamState::copyLabelSnapshot(LabelSnapshot& out) const {
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snap.gridWidth = m_gridWidth;
        snap.gridHeight = m_gridHeight;
        snap.appendMs = m_appendMs;
        snap.lastSliceStartMs = m_lastSliceStartMs;
        snap.timeOriginMs = m_timeOriginMs;
        snap.streamBaseMs = m_streamBaseMs;
        snap.filledColumns = m_filledColumns;
        snap.minPrice = m_minPrice;
        snap.maxPrice = m_maxPrice;
        snap.tickSize = m_tickSize;
    }
    snap.timeOffset = m_timeOffset.load();

    std::lock_guard<std::mutex> lock(m_ringMutex);
    snap.liquidityAvailable = m_liquidityAvailable;
    out.snapshot = snap;
    out.liquidityRing = m_liquidityRing;
    out.intensityRing = m_intensityRing;
    out.liquidityScales = m_liquidityScales;
    return snap.liquidityAvailable;
}

void HeatmapStreamState::takePendingUploads(std::vector<PendingColumn>& out) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    if (!m_pendingUploads.empty()) {
        out.swap(m_pendingUploads);
    }
}

void HeatmapStreamState::injectPendingUploads(std::vector<PendingColumn>&& columns) {
    std::lock_guard<std::mutex> lock(m_uploadMutex);
    m_pendingUploads = std::move(columns);
}

void HeatmapStreamState::takePendingLabelUploads(std::vector<PendingLabelColumn>& out) {
    std::lock_guard<std::mutex> lock(m_labelUploadMutex);
    if (!m_pendingLabelUploads.empty()) {
        out.swap(m_pendingLabelUploads);
    }
}

void HeatmapStreamState::copyLiquiditySnapshot(std::vector<uint16_t>& liquidityRing,
                                               std::vector<uint16_t>& intensityRing,
                                               std::vector<double>& liquidityScales,
                                               bool& liquidityAvailable) const {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    liquidityRing = m_liquidityRing;
    intensityRing = m_intensityRing;
    liquidityScales = m_liquidityScales;
    liquidityAvailable = m_liquidityAvailable;
}

void HeatmapStreamState::resetLocked(int gridWidth, int gridHeight) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_writeColumn = 0;
    m_filledColumns = 0;
    m_lastAppendMs = 0;
    m_lastSliceStartMs = std::numeric_limits<int64_t>::min();
    m_timeOriginMs = 0;
    m_streamBaseMs = std::numeric_limits<int64_t>::min();
    m_minPrice = 0.0;
    m_maxPrice = 0.0;
    m_tickSize = 0.0;

    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        m_pendingUploads.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_labelUploadMutex);
        m_pendingLabelUploads.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        const size_t expectedSize = static_cast<size_t>(gridWidth) * gridHeight;
        m_intensityRing.assign(expectedSize, 0);
        m_liquidityRing.assign(expectedSize, 0);
        m_liquidityScales.assign(gridWidth, 1.0);
        m_liquidityAvailable = false;
    }
    m_timeOffset.store(0.0f);
}
