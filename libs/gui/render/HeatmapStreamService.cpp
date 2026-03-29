// HeatmapStreamService — heatmap ring-buffer lifecycle, ingestion, render tick.
#include "HeatmapStreamService.hpp"

#include "SentinelLogging.hpp"
#include "render/GridViewState.hpp"
#include "render/HeatmapOverlayRenderer.hpp"
#include "render/ViewportAutoScrollController.hpp"

#include <QDateTime>
#include <QtEndian>
#include <algorithm>
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

// ── Column ingestion ─────────────────────────────────────────────────────────

HeatmapStreamService::IngestResult
HeatmapStreamService::ingestColumn(const HeatmapColumnEvent& event,
                                   GridViewState* viewState,
                                   HeatmapOverlayRenderer& overlay,
                                   int liquidityLabelMode,
                                   int64_t currentTimeframeMs) {
    const bool debug = qEnvironmentVariableIsSet("SENTINEL_GPU_HEATMAP_DEBUG");
    IngestResult result;

    if (event.column.isEmpty()) {
        if (debug) {
            sLog_Render("GPU HEATMAP DROP: grid=" << m_gridWidth << "x" << m_gridHeight
                        << " bytes=" << event.column.size());
        }
        return result;
    }

    // ── Intensity format update ──────────────────────────────────────────────
    const int bytesPerCell = (event.intensityBytesPerCell > 0) ? event.intensityBytesPerCell : 1;
    if (bytesPerCell != m_intensityBytesPerCell) {
        m_intensityBytesPerCell = bytesPerCell;
        overlay.setIntensityBytesPerCell(bytesPerCell);
        if (m_stream) {
            m_stream->setIntensityBytesPerCell(bytesPerCell);
        }
    }
    if ((event.column.size() % bytesPerCell) != 0) {
        return result;
    }
    const int columnHeight = event.column.size() / bytesPerCell;
    if (columnHeight <= 0) {
        return result;
    }

    // ── Grid height change → full reset ──────────────────────────────────────
    if (columnHeight != m_gridHeight) {
        m_gridHeight = columnHeight;
        overlay.setGridDimensions(m_gridWidth, m_gridHeight);
        if (m_stream) {
            m_stream->reset(m_gridWidth, m_gridHeight,
                            event.minPrice, event.maxPrice, event.tickSize);
        }
        m_viewportInitialized = false;
    }

    // ── Debug statistics ─────────────────────────────────────────────────────
    if (debug) {
        static int debugCount = 0;
        ++debugCount;
        if (debugCount <= 5 || debugCount % 50 == 0) {
            int bidCount = 0;
            int askCount = 0;
            uint16_t minValue = std::numeric_limits<uint16_t>::max();
            uint16_t maxValue = 0;
            if (bytesPerCell == 1) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(event.column.constData());
                for (int i = 0; i < event.column.size(); ++i) {
                    const uint8_t v = bytes[i];
                    if (v == 0) continue;
                    minValue = std::min<uint16_t>(minValue, static_cast<uint16_t>(v) * 257);
                    maxValue = std::max<uint16_t>(maxValue, static_cast<uint16_t>(v) * 257);
                    if (v >= 128) ++askCount;
                    else ++bidCount;
                }
            } else if (bytesPerCell == 2) {
                const auto* values = reinterpret_cast<const uint16_t*>(event.column.constData());
                for (int i = 0; i < columnHeight; ++i) {
                    const uint16_t v = qFromLittleEndian(values[i]);
                    if (v == 0) continue;
                    minValue = std::min(minValue, v);
                    maxValue = std::max(maxValue, v);
                    if (v >= 0x8000u) ++askCount;
                    else ++bidCount;
                }
            }
            sLog_Render("GPU HEATMAP BYTES: bids=" << bidCount
                        << " asks=" << askCount
                        << " min=" << minValue
                        << " max=" << maxValue
                        << " bpp=" << bytesPerCell);
        }
    }

    // ── Tick size tracking ───────────────────────────────────────────────────
    if (event.tickSize > 0.0 && event.tickSize != m_tickSize) {
        m_tickSize = event.tickSize;
        result.tickSizeChanged = true;
        result.newTickSize = m_tickSize;
    }

    // ── Cadence resolution ───────────────────────────────────────────────────
    int64_t cadenceMs = m_timeAuthority.activeTimeframeMs();
    if (cadenceMs <= 0) {
        cadenceMs = (event.timeframeMs > 0) ? event.timeframeMs : currentTimeframeMs;
        m_timeAuthority.setActiveTimeframeMs(cadenceMs);
    }

    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        static QElapsedTimer cadenceTimer;
        static bool cadenceTimerStarted = false;
        if (!cadenceTimerStarted) {
            cadenceTimer.start();
            cadenceTimerStarted = true;
        }
        if (cadenceTimer.elapsed() > 1000 &&
            event.timeframeMs > 0 &&
            cadenceMs > 0 &&
            event.timeframeMs != cadenceMs) {
            sLog_Debug(QString("Cadence mismatch: incoming_slice_tf=%1ms active_mapping_tf=%2ms renderer_tf=%3ms")
                           .arg(event.timeframeMs)
                           .arg(cadenceMs)
                           .arg(currentTimeframeMs));
            cadenceTimer.restart();
        }
    }

    // ── Stream range + append cadence ────────────────────────────────────────
    if (m_stream) {
        m_stream->updateRange(event.minPrice, event.maxPrice, event.tickSize);
        if (cadenceMs > 0) {
            m_stream->setAppendMs(static_cast<int>(cadenceMs));
        }
    }

    // ── Liquidity tracking ───────────────────────────────────────────────────
    const int expectedLiquidityBytes = m_gridHeight * static_cast<int>(sizeof(uint16_t));
    const bool haveLiquidityColumn = (event.liquidityColumn.size() == expectedLiquidityBytes);

    if (haveLiquidityColumn && event.liquidityScale > 0.0) {
        const auto* raw = reinterpret_cast<const uint16_t*>(event.liquidityColumn.constData());
        double colMin = std::numeric_limits<double>::max();
        double colMax = 0.0;
        int nonZeroCount = 0;
        for (int y = 0; y < m_gridHeight; ++y) {
            const uint16_t packed = qFromLittleEndian(raw[y]);
            if (packed == 0) continue;
            double value = static_cast<double>(packed) * event.liquidityScale;
            if (liquidityLabelMode != 0) {
                const double price = event.maxPrice - (static_cast<double>(y) * event.tickSize);
                value *= price;
            }
            if (value > colMax) colMax = value;
            if (value < colMin) colMin = value;
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

    // ── Slice ingestion ──────────────────────────────────────────────────────
    if (m_stream) {
        const qint64 nowMs = m_clock.elapsed();
        m_stream->ingestSlice(event.sliceStartMs,
                              static_cast<int>(cadenceMs),
                              event.column,
                              event.liquidityColumn,
                              event.liquidityScale,
                              nowMs);
        m_stream->updateTimeOffset(0.0f);
        m_timeAuthority.observeEventTime(
            (event.sliceEndMs > event.sliceStartMs)
                ? event.sliceEndMs
                : (event.sliceStartMs + cadenceMs),
            nowMs);
    }

    if (debug) {
        const int writeColumn = m_stream ? m_stream->writeColumn() : 0;
        sLog_Render("GPU HEATMAP ENQUEUE: col=" << writeColumn
                    << " tf=" << event.timeframeMs
                    << " range=$" << event.minPrice << "-$" << event.maxPrice);
    }

    // ── Viewport initialization ──────────────────────────────────────────────
    if (!m_viewportInitialized && viewState && m_stream && m_autoScrollController) {
        if (m_autoScrollController->initializeViewport(*viewState,
                                                       *m_stream,
                                                       event.sliceStartMs,
                                                       static_cast<int>(cadenceMs))) {
            m_viewportInitialized = true;
            // viewport initialized successfully
            if (debug) {
                const auto snapshot = m_stream->snapshot();
                sLog_Render("GPU HEATMAP VIEWPORT INIT: [" << viewState->getVisibleTimeStart()
                            << "-" << viewState->getVisibleTimeEnd()
                            << "] $" << snapshot.minPrice << "-$" << snapshot.maxPrice);
            }
        }
    }

    // ── Slice auto-scroll (non-smooth mode) ──────────────────────────────────
    if (viewState && viewState->isAutoScrollEnabled() && m_stream && m_autoScrollController &&
        !m_autoScrollController->smoothEnabled()) {
        const bool applied = m_autoScrollController->applySliceAutoScroll(*viewState,
                                                                          *m_stream,
                                                                          event.sliceStartMs,
                                                                          static_cast<int>(cadenceMs));
        if (applied) {
            result.autoScrollApplied = true;
        }
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

void HeatmapStreamService::handleTimeframeChange(int64_t timeframeMs) {
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

    ensureClockStarted();

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
    if (m_autoScrollController) {
        m_autoScrollController->resetSpan();
    }
    m_viewportInitialized = false;

    // Set an initial viewport if data range is valid
    if (viewState && minPrice < maxPrice && m_gridWidth > 0) {
        const int64_t cadenceMs = (m_timeAuthority.activeTimeframeMs() > 0)
            ? m_timeAuthority.activeTimeframeMs()
            : 1000;
        const int pct = m_autoScrollController
            ? std::clamp(m_autoScrollController->initialViewportPct(), 1, 100)
            : 10;
        const int64_t maxSpanMs = static_cast<int64_t>(m_gridWidth) * cadenceMs;
        const int64_t spanMs = std::max<int64_t>(1, maxSpanMs * pct / 100);
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

void HeatmapStreamService::setInitialViewportPct(int pct) {
    if (m_autoScrollController) {
        m_autoScrollController->setInitialViewportPct(pct);
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
        columns.push_back({x, std::move(intensityData), std::move(liquidityData), liqScale});
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
