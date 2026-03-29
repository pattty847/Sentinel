/*
Sentinel — ViewportAutoScrollController
*/
#include "ViewportAutoScrollController.hpp"

#include "GridViewState.hpp"
#include "HeatmapStreamState.hpp"

#include <algorithm>
#include <limits>

namespace {
int64_t clampSpanMs(int64_t spanMs, int64_t maxSpanMs) {
    if (maxSpanMs <= 0) {
        return spanMs;
    }
    return std::min(spanMs, maxSpanMs);
}
} // namespace

void ViewportAutoScrollController::setPaddingFrac(double fraction) {
    m_paddingFrac = std::clamp(fraction, 0.0, 0.45);
}

void ViewportAutoScrollController::setSmoothEnabled(bool enabled) {
    m_smoothEnabled = enabled;
}

void ViewportAutoScrollController::setInitialViewportPct(int pct) {
    m_initialViewportPct = std::clamp(pct, 1, 100);
}

void ViewportAutoScrollController::setInitialPricePct(int pct) {
    m_initialPricePct = std::clamp(pct, 0, 100);
}

void ViewportAutoScrollController::resetSpan() {
    m_autoScrollSpanMs = 0;
    m_lastViewEndMs = std::numeric_limits<int64_t>::min();
}

void ViewportAutoScrollController::updateLagFromView(const GridViewState& view,
                                                     const HeatmapStreamState& stream) {
    const auto snapshot = stream.snapshot();
    updateLagFromView(view, stream, snapshot.appendMs);
}

void ViewportAutoScrollController::updateLagFromView(const GridViewState& view,
                                                     const HeatmapStreamState& stream,
                                                     int64_t timeframeMs,
                                                     int64_t steadyNowMs) {
    const auto snapshot = stream.snapshot();
    if (timeframeMs <= 0 || snapshot.gridWidth <= 0 ||
        snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return;
    }
    const int64_t spanMs = std::max<int64_t>(1, view.getVisibleTimeEnd() - view.getVisibleTimeStart());
    const int64_t padMs = static_cast<int64_t>(spanMs * m_paddingFrac);
    int64_t anchorTimeMs = snapshot.lastSliceStartMs + timeframeMs;
    if (m_smoothEnabled &&
        steadyNowMs != std::numeric_limits<int64_t>::min() &&
        snapshot.streamBaseMs != std::numeric_limits<int64_t>::min()) {
        anchorTimeMs = snapshot.streamBaseMs + steadyNowMs + timeframeMs;
    }
    m_autoScrollLagMs = (view.getVisibleTimeEnd() - anchorTimeMs) - padMs;
    const int64_t maxSpanMs = static_cast<int64_t>(snapshot.gridWidth) * timeframeMs;
    m_autoScrollSpanMs = std::min(spanMs, maxSpanMs);
    m_lastViewEndMs = view.getVisibleTimeEnd();
}

bool ViewportAutoScrollController::initializeViewport(GridViewState& view,
                                                      const HeatmapStreamState& stream,
                                                      int64_t sliceStartMs,
                                                      int timeframeMs) {
    const auto snapshot = stream.snapshot();
    if (timeframeMs <= 0 || snapshot.gridWidth <= 0) {
        return false;
    }
    const int64_t maxSpanMs = std::max<int64_t>(1, static_cast<int64_t>(snapshot.gridWidth - 1) * timeframeMs);
    if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
        const double pct = static_cast<double>(std::clamp(m_initialViewportPct, 1, 100)) / 100.0;
        m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * pct * (1.0 - m_paddingFrac));
        if (m_autoScrollSpanMs <= 0) {
            m_autoScrollSpanMs = maxSpanMs;
        }
    }
    const int64_t spanMs = m_autoScrollSpanMs;
    const int64_t padMs = static_cast<int64_t>(spanMs * m_paddingFrac);
    const int64_t viewEnd = sliceStartMs + timeframeMs + padMs;
    const int64_t viewStart = viewEnd - spanMs;
    if (viewEnd <= viewStart) {
        return false;
    }
    double viewMinPrice = snapshot.minPrice;
    double viewMaxPrice = snapshot.maxPrice;
    if (m_initialPricePct > 0 && m_initialPricePct < 100 &&
        snapshot.maxPrice > snapshot.minPrice) {
        const double mid = (snapshot.minPrice + snapshot.maxPrice) * 0.5;
        const double fullRange = snapshot.maxPrice - snapshot.minPrice;
        const double viewRange = fullRange * (static_cast<double>(m_initialPricePct) / 100.0);
        viewMinPrice = mid - viewRange * 0.5;
        viewMaxPrice = mid + viewRange * 0.5;
    }
    view.setViewport(viewStart, viewEnd, viewMinPrice, viewMaxPrice);
    m_lastViewEndMs = viewEnd;
    return true;
}

bool ViewportAutoScrollController::applySliceAutoScroll(GridViewState& view,
                                                        const HeatmapStreamState& stream,
                                                        int64_t sliceStartMs,
                                                        int timeframeMs) {
    if (timeframeMs <= 0 || view.isDragging()) {
        return false;
    }
    const auto snapshot = stream.snapshot();
    if (snapshot.gridWidth <= 0 || snapshot.appendMs <= 0) {
        return false;
    }
    const int64_t maxSpanMs = static_cast<int64_t>(snapshot.gridWidth) * timeframeMs;
    if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
        m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * (1.0 - m_paddingFrac));
        if (m_autoScrollSpanMs <= 0) {
            m_autoScrollSpanMs = maxSpanMs;
        }
    }
    const int64_t clampedSpanMs = clampSpanMs(m_autoScrollSpanMs, maxSpanMs);
    const int64_t padMs = static_cast<int64_t>(clampedSpanMs * m_paddingFrac);
    int64_t viewEnd = sliceStartMs + timeframeMs + m_autoScrollLagMs + padMs;
    if (m_lastViewEndMs != std::numeric_limits<int64_t>::min() && viewEnd < m_lastViewEndMs) {
        viewEnd = m_lastViewEndMs;
    }
    const int64_t viewStart = viewEnd - clampedSpanMs;

    const double priceSpan = std::max(1e-6, view.getMaxPrice() - view.getMinPrice());
    const double heatmapMid = (snapshot.minPrice + snapshot.maxPrice) * 0.5;
    double viewMin = view.getMinPrice();
    double viewMax = view.getMaxPrice();
    if (viewMax < snapshot.minPrice || viewMin > snapshot.maxPrice) {
        viewMin = heatmapMid - priceSpan * 0.5;
        viewMax = heatmapMid + priceSpan * 0.5;
    }
    view.setViewport(viewStart, viewEnd, viewMin, viewMax);
    m_lastViewEndMs = viewEnd;
    return true;
}

bool ViewportAutoScrollController::applySmoothAutoScroll(GridViewState& view,
                                                         const HeatmapStreamState& stream,
                                                         int64_t nowMs,
                                                         int64_t timeframeMs) {
    if (view.isDragging()) {
        return false;
    }
    const auto snapshot = stream.snapshot();
    if (timeframeMs <= 0 || snapshot.gridWidth <= 0 ||
        snapshot.lastSliceStartMs == std::numeric_limits<int64_t>::min()) {
        return false;
    }
    const int64_t maxSpanMs = static_cast<int64_t>(snapshot.gridWidth) * timeframeMs;
    if (m_autoScrollSpanMs <= 0 || m_autoScrollSpanMs > maxSpanMs) {
        m_autoScrollSpanMs = static_cast<int64_t>(maxSpanMs * (1.0 - m_paddingFrac));
        if (m_autoScrollSpanMs <= 0) {
            m_autoScrollSpanMs = maxSpanMs;
        }
    }
    const int64_t clampedSpanMs = clampSpanMs(m_autoScrollSpanMs, maxSpanMs);
    const int64_t padMs = static_cast<int64_t>(clampedSpanMs * m_paddingFrac);
    const int64_t streamNowMs = (snapshot.streamBaseMs != std::numeric_limits<int64_t>::min())
        ? (snapshot.streamBaseMs + nowMs + timeframeMs)
        : (snapshot.lastSliceStartMs + timeframeMs);
    int64_t viewEnd = streamNowMs + m_autoScrollLagMs + padMs;
    if (m_lastViewEndMs != std::numeric_limits<int64_t>::min() && viewEnd < m_lastViewEndMs) {
        viewEnd = m_lastViewEndMs;
    }
    const int64_t viewStart = viewEnd - clampedSpanMs;

    const double priceSpan = std::max(1e-6, view.getMaxPrice() - view.getMinPrice());
    const double heatmapMid = (snapshot.minPrice + snapshot.maxPrice) * 0.5;
    double viewMin = view.getMinPrice();
    double viewMax = view.getMaxPrice();
    if (viewMax < snapshot.minPrice || viewMin > snapshot.maxPrice) {
        viewMin = heatmapMid - priceSpan * 0.5;
        viewMax = heatmapMid + priceSpan * 0.5;
    }

    view.setViewport(viewStart, viewEnd, viewMin, viewMax);
    m_lastViewEndMs = viewEnd;
    return true;
}

bool ViewportAutoScrollController::applyPanToAutoScroll(GridViewState& view,
                                                        double viewportWidth,
                                                        double viewportHeight) {
    const QPointF pan = view.getPanVisualOffset();
    if (pan.isNull() || viewportWidth <= 0.0 || viewportHeight <= 0.0) {
        return false;
    }
    const int64_t timeRange = view.getVisibleTimeEnd() - view.getVisibleTimeStart();
    const double priceRange = view.getMaxPrice() - view.getMinPrice();
    if (timeRange == 0 || priceRange == 0.0) {
        return false;
    }
    const double timePixelsToMs = static_cast<double>(timeRange) / viewportWidth;
    const double pricePixelsToUnits = priceRange / viewportHeight;
    const double timeDeltaF = (-pan.x() * timePixelsToMs);
    const int64_t timeDelta = static_cast<int64_t>(std::floor(timeDeltaF));
    const double priceDelta = pan.y() * pricePixelsToUnits;
    m_autoScrollLagMs += timeDelta;
    if (timeDelta != 0 || priceDelta != 0.0) {
        const qint64 newStart = view.getVisibleTimeStart() + timeDelta;
        const qint64 newEnd = view.getVisibleTimeEnd() + timeDelta;
        const double newMin = view.getMinPrice() + priceDelta;
        const double newMax = view.getMaxPrice() + priceDelta;
        view.setViewport(newStart, newEnd, newMin, newMax);
        m_lastViewEndMs = newEnd;
    }
    return true;
}
