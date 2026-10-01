// HeatmapStreamService — heatmap ring-buffer lifecycle, ingestion, render tick.
#include "HeatmapStreamService.hpp"

#include "SentinelLogging.hpp"
#include "render/GridViewState.hpp"
#include "render/HeatmapOverlayRenderer.hpp"
#include "render/ViewportAutoScrollController.hpp"

#include <QDateTime>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>

HeatmapStreamService::HeatmapStreamService(QObject* parent)
    : QObject(parent) {}

HeatmapStreamService::~HeatmapStreamService() = default;

void HeatmapStreamService::init(int gridWidth, int gridHeight,
                                int64_t timeframeMs, int intensityBytesPerCell) {
    m_gridWidth = gridWidth;
    m_gridHeight = gridHeight;
    m_intensityBytesPerCell = intensityBytesPerCell;

    m_clock.start();
    m_stream = std::make_unique<HeatmapStreamState>();
    m_timeAuthority.setActiveTimeframeMs(timeframeMs);
    m_stream->setGridDimensions(m_gridWidth, m_gridHeight);
    m_stream->setAppendMs(static_cast<int>(timeframeMs));
    m_stream->setIntensityBytesPerCell(m_intensityBytesPerCell);

    m_autoScrollController = std::make_unique<ViewportAutoScrollController>();
}

void HeatmapStreamService::ensureClockStarted() {
    if (!m_clock.isValid()) {
        m_clock.start();
    }
}

// ── Window updates ───────────────────────────────────────────────────────────

HeatmapStreamService::IngestResult
HeatmapStreamService::applyWindowUpdate(const heatmap_window::Update& update,
                                        GridViewState* viewState,
                                        HeatmapOverlayRenderer& overlay,
                                        int liquidityLabelMode) {
    IngestResult result;
    if (!m_stream || update.width <= 0 || update.rows <= 0 || update.timeframeMs <= 0 ||
        (update.bytesPerCell != 1 && update.bytesPerCell != 2) || !update.band.valid()) {
        sLog_RenderN(1000, "Heatmap window update rejected: stream=" << (m_stream != nullptr)
                     << " grid=" << update.width << "x" << update.rows
                     << " tf=" << update.timeframeMs << " bytesPerCell=" << update.bytesPerCell
                     << " band=[" << update.band.minPrice << ".." << update.band.maxPrice << "]"
                     << " tick=" << update.band.tickSize);
        return result;
    }
    const int64_t cadenceMs = update.timeframeMs;

    const bool reshape = update.width != m_gridWidth || update.rows != m_gridHeight ||
                         update.bytesPerCell != m_intensityBytesPerCell;
    if (reshape) {
        sLog_Render("Heatmap texture reshape: grid=" << m_gridWidth << "x" << m_gridHeight
                    << "->" << update.width << "x" << update.rows
                    << " bytesPerCell=" << m_intensityBytesPerCell << "->" << update.bytesPerCell);
        m_gridWidth = update.width;
        m_gridHeight = update.rows;
        m_intensityBytesPerCell = update.bytesPerCell;
        overlay.setGridDimensions(m_gridWidth, m_gridHeight);
        overlay.setIntensityBytesPerCell(m_intensityBytesPerCell);
    }
    if (reshape || update.full) {
        overlay.requestFullTextureRebuild();
    }
    if (m_timeAuthority.activeTimeframeMs() != cadenceMs) {
        m_timeAuthority.setActiveTimeframeMs(cadenceMs);
    }

    // ── Liquidity range tracking (newest live column only) ───────────────────
    const heatmap_window::SlotWrite* liveWrite = nullptr;
    if (update.liveBucketMs > 0) {
        for (const auto& write : update.writes) {
            if (write.bucketStartMs == update.liveBucketMs) {
                liveWrite = &write;
                break;
            }
        }
    }
    const int expectedLiquidityBytes = m_gridHeight * static_cast<int>(sizeof(uint16_t));
    if (liveWrite && liveWrite->liquidity.size() == expectedLiquidityBytes &&
        liveWrite->liquidityScale > 0.0) {
        const auto* raw = reinterpret_cast<const uint16_t*>(liveWrite->liquidity.constData());
        double colMin = std::numeric_limits<double>::max();
        double colMax = 0.0;
        int nonZeroCount = 0;
        for (int y = 0; y < m_gridHeight; ++y) {
            const uint16_t packed = qFromLittleEndian(raw[y]);
            if (packed == 0) continue;
            double value = static_cast<double>(packed) * liveWrite->liquidityScale;
            if (liquidityLabelMode != 0) {
                value *= update.band.maxPrice - (static_cast<double>(y) * update.band.tickSize);
            }
            colMax = std::max(colMax, value);
            colMin = std::min(colMin, value);
            ++nonZeroCount;
        }
        if (colMax > m_maxObservedLiquidity) {
            m_maxObservedLiquidity = colMax;
            result.maxLiquidityChanged = true;
            result.newMaxLiquidity = m_maxObservedLiquidity;
        }
        if (nonZeroCount > 0 && colMin < m_minObservedLiquidity) {
            m_minObservedLiquidity = colMin;
            result.minLiquidityChanged = true;
            result.newMinLiquidity = m_minObservedLiquidity;
        }
    }

    // ── Ring window ──────────────────────────────────────────────────────────
    std::vector<HeatmapStreamState::SlotColumn> slotColumns;
    slotColumns.reserve(update.writes.size());
    for (const auto& write : update.writes) {
        slotColumns.push_back({write.slot, write.intensity, write.liquidity, write.liquidityScale, write.validity});
    }
    HeatmapStreamState::WindowPlacement placement;
    placement.valueEncoding = update.valueEncoding;
    placement.bandGeneration = update.bandGeneration;
    placement.sizeFloor = update.sizeFloor;
    placement.codesPerOctave = update.codesPerOctave;
    placement.timeframeMs = cadenceMs;
    placement.gridWidth = update.width;
    placement.gridHeight = update.rows;
    placement.bytesPerCell = update.bytesPerCell;
    placement.minPrice = update.band.minPrice;
    placement.maxPrice = update.band.maxPrice;
    placement.tickSize = update.band.tickSize;
    placement.windowEndMs = update.windowEndMs;
    placement.newestSlot = update.newestSlot;
    placement.full = update.full;
    placement.liveEdge = update.pinnedToLive && update.liveBucketMs == update.windowEndMs;
    const qint64 nowMs = m_clock.elapsed();
    if (!m_stream->applyWindow(placement, std::move(slotColumns), nowMs)) {
        sLog_RenderN(1000, "Heatmap window update dropped by ring: grid=" << update.width << "x"
                     << update.rows << " newestSlot=" << update.newestSlot
                     << " writes=" << update.writes.size() << " end=" << update.windowEndMs);
        return result;
    }
    m_stream->updateTimeOffset(0.0f);
    m_priceBandReady = true;
    overlay.setHistoryCoverage(update.coverage);

    if (update.band.tickSize != m_tickSize) {
        m_tickSize = update.band.tickSize;
        result.tickSizeChanged = true;
        result.newTickSize = m_tickSize;
    }
    if (update.liveBucketMs > 0) {
        m_timeAuthority.observeEventTime(update.liveBucketMs + cadenceMs, nowMs);
    }
    sLog_Probe("heatmap.window", "tf=" << cadenceMs << " end=" << update.windowEndMs
               << " grid=" << update.width << "x" << update.rows
               << " newestSlot=" << update.newestSlot << " writes=" << update.writes.size()
               << " full=" << update.full << " pinned=" << update.pinnedToLive
               << " live=" << update.liveBucketMs
               << " band=[" << update.band.minPrice << ".." << update.band.maxPrice << "]"
               << " tick=" << update.band.tickSize);

    // ── Viewport initialization and non-smooth follow ────────────────────────
    const int64_t anchorMs = update.liveBucketMs > 0 ? update.liveBucketMs : update.windowEndMs;
    if (!m_viewportInitialized && viewState && m_autoScrollController &&
        (viewState->isAutoScrollEnabled() || m_autoScrollController->priceCenterPending())) {
        m_viewportInitialized = m_autoScrollController->initializeViewport(
            *viewState, *m_stream, anchorMs, static_cast<int>(cadenceMs));
    }
    if (update.liveBucketMs > 0 && viewState && viewState->isAutoScrollEnabled() &&
        m_autoScrollController && !m_autoScrollController->smoothEnabled() &&
        m_autoScrollController->applySliceAutoScroll(*viewState, *m_stream,
                                                     update.liveBucketMs,
                                                     static_cast<int>(cadenceMs))) {
        result.autoScrollApplied = true;
    }

    m_streamGeneration.fetch_add(1, std::memory_order_acq_rel);
    result.accepted = true;
    return result;
}

// ── Render loop tick ─────────────────────────────────────────────────────────

HeatmapStreamService::RenderTickResult
HeatmapStreamService::handleRenderTick(GridViewState* viewState) {
    RenderTickResult result;

    const auto snapshot = m_stream ? m_stream->snapshot() : HeatmapStreamState::Snapshot{};
    // S3 history has a fixed right edge; never advance presentation time past it.
    if (snapshot.valueEncoding == heatmap_window::ValueEncoding::AbsoluteLogSize) return result;
    const qint64 nowMs = m_clock.elapsed();
    const auto timeSnapshot = m_timeAuthority.snapshot(nowMs);
    const int64_t cadenceMs = (timeSnapshot.activeTimeframeMs > 0)
        ? timeSnapshot.activeTimeframeMs
        : static_cast<int64_t>(snapshot.appendMs);
    if (snapshot.gridWidth <= 0 || snapshot.gridHeight <= 0 || cadenceMs <= 0) {
        return result;
    }

    // Fractional time offset for inter-frame smoothness
    const bool useFractionalOffset = (viewState && viewState->isAutoScrollEnabled() &&
                                      m_autoScrollController && !m_autoScrollController->smoothEnabled());
    const qint64 lastAppendMs = m_stream ? m_stream->lastAppendMs() : 0;
    const qint64 delta = nowMs - lastAppendMs;
    const float frac = useFractionalOffset
        ? std::clamp(static_cast<float>(delta) / static_cast<float>(cadenceMs), 0.0f, 1.0f)
        : 0.0f;
    if (m_stream) {
        m_stream->updateTimeOffset(frac);
    }

    // Smooth auto-scroll
    const bool dragging = (viewState && viewState->isDragging());
    if (!dragging && m_autoScrollController && m_autoScrollController->smoothEnabled() &&
        viewState && viewState->isAutoScrollEnabled() && m_stream) {
        const bool applied = m_autoScrollController->applySmoothAutoScroll(*viewState,
                                                                           *m_stream,
                                                                           nowMs,
                                                                           cadenceMs);
        if (applied) {
            result.autoScrollApplied = true;
        }
    }

    result.shouldUpdate = true;
    return result;
}

// ── Timeframe change ─────────────────────────────────────────────────────────

void HeatmapStreamService::handleTimeframeChange(int64_t timeframeMs,
                                                 HeatmapOverlayRenderer& overlay) {
    overlay.setHistoryCoverage({});
    m_timeAuthority.setActiveTimeframeMs(timeframeMs);
    if (m_stream) {
        const auto snap = m_stream->snapshot();
        m_stream->reset(snap.gridWidth > 0 ? snap.gridWidth : m_gridWidth,
                        snap.gridHeight > 0 ? snap.gridHeight : m_gridHeight,
                        snap.minPrice, snap.maxPrice, snap.tickSize);
        m_stream->setAppendMs(static_cast<int>(timeframeMs));
    }
    if (m_autoScrollController) {
        m_autoScrollController->resetSpan();
    }
    m_viewportInitialized = false;
}

// ── Range reset ──────────────────────────────────────────────────────────────

HeatmapStreamService::RangeResetResult
HeatmapStreamService::handleRangeReset(double minPrice, double maxPrice, double tickSize,
                                        int gridWidth, int gridHeight,
                                        GridViewState* viewState,
                                        HeatmapOverlayRenderer& overlay) {
    RangeResetResult result;

    sLog_Render("Heatmap range reset: band=[" << minPrice << ".." << maxPrice << "] tick=" << tickSize
                << " grid=" << gridWidth << "x" << gridHeight);
    ensureClockStarted();

    overlay.setHistoryCoverage({});

    if (gridWidth > 0) m_gridWidth = gridWidth;
    if (gridHeight > 0) m_gridHeight = gridHeight;

    overlay.setGridDimensions(m_gridWidth, m_gridHeight);
    overlay.requestFullTextureRebuild();
    m_streamGeneration.fetch_add(1, std::memory_order_acq_rel);

    if (tickSize > 0.0 && tickSize != m_tickSize) {
        m_tickSize = tickSize;
        result.tickSizeChanged = true;
        result.newTickSize = tickSize;
    }

    if (m_stream) {
        m_stream->reset(m_gridWidth, m_gridHeight, minPrice, maxPrice, tickSize);
    }
    m_priceBandReady = minPrice < maxPrice && std::isfinite(minPrice) && std::isfinite(maxPrice);
    if (m_autoScrollController) {
        m_autoScrollController->resetSpan();
    }
    // Preserve an established view through band changes, including manual price pans.
    if (viewState && !viewState->isTimeWindowValid() && minPrice < maxPrice && m_gridWidth > 0) {
        const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
            ? m_timeAuthority.activeTimeframeMs()
            : 1000;
        const int64_t spanMs = m_autoScrollController
            ? m_autoScrollController->initialSpanMs(viewState->getViewportWidth(), m_gridWidth, cadenceMs)
            : static_cast<int64_t>(100) * cadenceMs;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        viewState->setViewport(nowMs - spanMs, nowMs, minPrice, maxPrice);
    }

    return result;
}

// ── Auto-scroll configuration ────────────────────────────────────────────────

void HeatmapStreamService::setAutoScrollPaddingFrac(double frac) {
    if (m_autoScrollController) {
        m_autoScrollController->setPaddingFrac(frac);
        m_autoScrollController->resetSpan();
    }
}

void HeatmapStreamService::setAutoScrollSmoothEnabled(bool enabled) {
    if (m_autoScrollController) {
        m_autoScrollController->setSmoothEnabled(enabled);
    }
}

void HeatmapStreamService::setInitialColumnPx(int px) {
    if (m_autoScrollController) {
        m_autoScrollController->setInitialColumnPx(px);
    }
}

void HeatmapStreamService::setInitialPricePct(int pct) {
    if (m_autoScrollController) {
        m_autoScrollController->setInitialPricePct(pct);
    }
}

void HeatmapStreamService::resetAutoScrollSpan() {
    if (m_autoScrollController) {
        m_autoScrollController->resetSpan();
    }
}

void HeatmapStreamService::resetPriceCenter() {
    if (m_autoScrollController) m_autoScrollController->resetPriceCenter();
    m_viewportInitialized = false;
    m_priceBandReady = false;
}

void HeatmapStreamService::adoptViewport(const GridViewState& viewState) {
    if (!viewState.isTimeWindowValid()) return; // nothing established: bootstrap normally
    m_viewportInitialized = true;
    if (m_autoScrollController) {
        m_autoScrollController->adoptView(viewState.getVisibleTimeEnd() - viewState.getVisibleTimeStart(),
                                          viewState.getVisibleTimeEnd());
    }
}

void HeatmapStreamService::setLiveBook(double bid, double ask, GridViewState* viewState) {
    if (!m_autoScrollController) return;
    m_autoScrollController->setLiveBook(bid, ask);
    applyPendingPriceCenter(viewState);
}

void HeatmapStreamService::setLastTrade(double price, GridViewState* viewState) {
    if (!m_autoScrollController) return;
    m_autoScrollController->setLastTrade(price);
    applyPendingPriceCenter(viewState);
}

void HeatmapStreamService::requestPriceCenter(GridViewState* viewState) {
    if (!m_autoScrollController) return;
    m_autoScrollController->requestPriceCenter();
    applyPendingPriceCenter(viewState);
}

void HeatmapStreamService::cancelPriceCenter() {
    if (m_autoScrollController) m_autoScrollController->cancelPriceCenter();
}

bool HeatmapStreamService::recordingViewportReady(GridViewState* viewState) {
    if (!m_recordingMode || !m_autoScrollController ||
        !m_autoScrollController->initialPriceCenterPending()) return true;
    applyPendingPriceCenter(viewState);
    return !m_autoScrollController->initialPriceCenterPending();
}

void HeatmapStreamService::applyPendingPriceCenter(GridViewState* viewState) {
    if (!viewState || !m_autoScrollController || (!m_priceBandReady && !m_recordingMode) ||
        !m_autoScrollController->priceCenterPending()) return;
    const auto snapshot = m_stream ? m_stream->snapshot() : HeatmapStreamState::Snapshot{};
    double span = viewState->getMaxPrice() - viewState->getMinPrice();
    if (m_autoScrollController->initialPriceCenterPending() && snapshot.maxPrice > snapshot.minPrice &&
        std::isfinite(snapshot.maxPrice) && std::isfinite(snapshot.minPrice)) {
        span = snapshot.maxPrice - snapshot.minPrice;
        const int pct = m_autoScrollController->initialPricePct();
        if (pct > 0 && pct < 100) span *= static_cast<double>(pct) / 100.0;
    } else if (m_recordingMode && !m_priceBandReady &&
               m_autoScrollController->initialPriceCenterPending()) {
        const int pct = m_autoScrollController->initialPricePct();
        if (pct > 0 && pct < 100) span *= static_cast<double>(pct) / 100.0;
    }
    if (m_autoScrollController->applyPendingPriceCenter(*viewState, span)) {
        sLog_Render("Price viewport centered: price=[" << viewState->getMinPrice() << ".."
                    << viewState->getMaxPrice() << "] span=" << span);
    }
}

void HeatmapStreamService::updateAutoScrollLag(GridViewState& vs, int64_t cadenceMs) {
    if (m_autoScrollController && m_stream) {
        m_autoScrollController->updateLagFromView(
            vs, *m_stream, cadenceMs,
            m_clock.isValid() ? m_clock.elapsed()
                              : std::numeric_limits<int64_t>::min());
    }
}

// ── Grid dimensions ──────────────────────────────────────────────────────────

void HeatmapStreamService::setGridDimensions(int w, int h, HeatmapOverlayRenderer& overlay) {
    bool changed = false;
    if (w > 0 && w != m_gridWidth) { m_gridWidth = w; changed = true; }
    if (h > 0 && h != m_gridHeight) { m_gridHeight = h; changed = true; }
    if (changed) {
        overlay.setGridDimensions(m_gridWidth, m_gridHeight);
        overlay.requestFullTextureRebuild();
        if (m_stream) {
            m_stream->setGridDimensions(m_gridWidth, m_gridHeight);
        }
    }
}

// ── Texture rebuild from ring buffer ─────────────────────────────────────────

void HeatmapStreamService::rebuildTextureFromRing(HeatmapOverlayRenderer& overlay,
                                                    double liquidityThreshold,
                                                    int liquidityLabelMode) {
    if (!m_stream) return;
    HeatmapStreamState::LabelSnapshot snap;
    if (!m_stream->copyLabelSnapshot(snap)) {
        overlay.requestFullTextureRebuild();
        return;
    }
    const auto& ss = snap.snapshot;
    if (ss.gridWidth <= 0 || ss.gridHeight <= 0) return;

    std::vector<HeatmapStreamState::PendingColumn> columns;
    columns.reserve(ss.gridWidth);

    for (int x = 0; x < ss.gridWidth; ++x) {
        QByteArray intensityData;
        QByteArray liquidityData;
        if (m_intensityBytesPerCell == 1) {
            intensityData.resize(ss.gridHeight);
            auto* dst = reinterpret_cast<uint8_t*>(intensityData.data());
            liquidityData.resize(ss.gridHeight * static_cast<int>(sizeof(uint16_t)));
            auto* liqDst = reinterpret_cast<uint16_t*>(liquidityData.data());
            for (int y = 0; y < ss.gridHeight; ++y) {
                const uint16_t ringVal = snap.intensityRing[static_cast<size_t>(y) * ss.gridWidth + x];
                uint8_t cell = static_cast<uint8_t>(ringVal / 257);
                const uint16_t liqRaw = snap.liquidityRing[static_cast<size_t>(y) * ss.gridWidth + x];
                liqDst[y] = qToLittleEndian(liqRaw);
                if (liquidityThreshold > 0.0) {
                    if (liqRaw == 0) {
                        cell = 0;
                    } else {
                        double val = static_cast<double>(liqRaw) * snap.liquidityScales[x];
                        if (liquidityLabelMode != 0 && ss.tickSize > 0.0)
                            val *= (ss.maxPrice - static_cast<double>(y) * ss.tickSize);
                        if (val < liquidityThreshold) cell = 0;
                    }
                }
                dst[y] = cell;
            }
        } else {
            intensityData.resize(ss.gridHeight * 2);
            auto* dst = reinterpret_cast<uint16_t*>(intensityData.data());
            liquidityData.resize(ss.gridHeight * static_cast<int>(sizeof(uint16_t)));
            auto* liqDst = reinterpret_cast<uint16_t*>(liquidityData.data());
            for (int y = 0; y < ss.gridHeight; ++y) {
                const uint16_t ringVal = snap.intensityRing[static_cast<size_t>(y) * ss.gridWidth + x];
                uint16_t cell = ringVal;
                const uint16_t liqRaw = snap.liquidityRing[static_cast<size_t>(y) * ss.gridWidth + x];
                liqDst[y] = qToLittleEndian(liqRaw);
                if (liquidityThreshold > 0.0) {
                    if (liqRaw == 0) {
                        cell = 0;
                    } else {
                        double val = static_cast<double>(liqRaw) * snap.liquidityScales[x];
                        if (liquidityLabelMode != 0 && ss.tickSize > 0.0)
                            val *= (ss.maxPrice - static_cast<double>(y) * ss.tickSize);
                        if (val < liquidityThreshold) cell = 0;
                    }
                }
                dst[y] = qToLittleEndian(cell);
            }
        }
        const double liqScale = (x < static_cast<int>(snap.liquidityScales.size()))
                                    ? snap.liquidityScales[x]
                                    : 1.0;
        const QByteArray validity = x < static_cast<int>(snap.validity.size()) ? snap.validity[x] : QByteArray{};
        columns.push_back({x, std::move(intensityData), std::move(liquidityData), liqScale, validity});
    }

    m_stream->injectPendingUploads(std::move(columns));
    overlay.requestFullTextureRebuild();
}

// ── Fit viewport to full data range ──────────────────────────────────────────

bool HeatmapStreamService::fitToDataRange(GridViewState* viewState) {
    const auto snapshot = m_stream ? m_stream->snapshot() : HeatmapStreamState::Snapshot{};
    const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
                                  ? m_timeAuthority.activeTimeframeMs()
                                  : static_cast<int64_t>(snapshot.appendMs);
    if (!viewState || cadenceMs <= 0 || snapshot.gridWidth <= 0) return false;
    if (snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) return false;

    const int64_t bufferSpanMs = std::max<int64_t>(
        1, static_cast<int64_t>(snapshot.gridWidth) * cadenceMs);
    const int64_t dataEnd = snapshot.lastSliceStartMs + cadenceMs;
    const int64_t dataStart = dataEnd - bufferSpanMs;
    if (dataEnd <= dataStart) return false;

    if (viewState->isAutoScrollEnabled()) {
        viewState->enableAutoScroll(false);
    }
    viewState->setViewport(dataStart, dataEnd, snapshot.minPrice, snapshot.maxPrice);
    return true;
}
