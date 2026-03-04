/*
 * Sentinel – VolumeProfileState (implementation)
 */
#include "VolumeProfileState.hpp"
#include "../../core/protocol/VolumeProfileSlice.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>
#include <QtEndian>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool sessionParamsMatch(const VolumeProfileState::Snapshot& snap,
                                const VolumeProfileSlice& s) {
    return snap.sessionStartMs == s.sessionStartMs &&
           snap.sessionEndMs   == s.sessionEndMs   &&
           snap.gridHeight     == s.gridHeight      &&
           std::abs(snap.minPrice - s.minPrice) < 1e-9 &&
           std::abs(snap.tickSize - s.tickSize) < 1e-12;
}

// ─────────────────────────────────────────────────────────────────────────────
//  VolumeProfileState
// ─────────────────────────────────────────────────────────────────────────────
void VolumeProfileState::resetLocked(const VolumeProfileSlice& slice) {
    m_sessionStartMs = slice.sessionStartMs;
    m_sessionEndMs   = slice.sessionEndMs;
    m_sessionType    = slice.sessionType;
    m_minPrice       = slice.minPrice;
    m_maxPrice       = slice.maxPrice;
    m_tickSize       = slice.tickSize;
    m_gridHeight     = slice.gridHeight;
    m_bins.assign(static_cast<size_t>(m_gridHeight), 0.0f);
    m_va    = {};
    m_dirty = false;
    ++m_generation;
}

bool VolumeProfileState::ingestSlice(const VolumeProfileSlice& slice) {
    if (slice.gridHeight <= 0 || slice.tickSize <= 0.0 ||
        slice.sessionStartMs <= 0 || slice.sessionEndMs <= slice.sessionStartMs ||
        slice.volumeBinsF32.size() != static_cast<int>(slice.gridHeight) * 4) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // Detect session change → full reset
    const bool sessionChanged =
        (m_sessionStartMs != slice.sessionStartMs) ||
        (m_sessionEndMs   != slice.sessionEndMs)   ||
        (m_gridHeight     != slice.gridHeight)      ||
        std::abs(m_minPrice - slice.minPrice) > 1e-9 ||
        std::abs(m_tickSize - slice.tickSize) > 1e-12;

    if (sessionChanged || m_bins.empty()) {
        resetLocked(slice);
    }

    // Replace bin array with the authoritative data from the server.
    // The server accumulates all trades for the session → client just mirrors.
    const auto* src = reinterpret_cast<const float*>(slice.volumeBinsF32.constData());
    for (int i = 0; i < m_gridHeight; ++i) {
        float v;
        // Safely load LE float (handles unaligned access).
        std::memcpy(&v, src + i, sizeof(float));
        m_bins[static_cast<size_t>(i)] = v;
    }

    // Recompute value area.
    m_va    = computeValueArea(m_bins, m_minPrice, m_tickSize);
    m_dirty = true;
    ++m_generation;
    return true;
}

bool VolumeProfileState::takePendingBins(std::vector<float>& out, Snapshot& outSnap) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dirty || m_bins.empty()) {
        return false;
    }
    out       = m_bins;   // copy is intentional – render thread will own this
    outSnap.sessionStartMs = m_sessionStartMs;
    outSnap.sessionEndMs   = m_sessionEndMs;
    outSnap.sessionType    = m_sessionType;
    outSnap.minPrice       = m_minPrice;
    outSnap.maxPrice       = m_maxPrice;
    outSnap.tickSize       = m_tickSize;
    outSnap.gridHeight     = m_gridHeight;
    outSnap.va             = m_va;
    outSnap.dirty          = true;
    outSnap.generation     = m_generation;
    m_dirty = false;
    return true;
}

VolumeProfileState::Snapshot VolumeProfileState::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot snap;
    snap.sessionStartMs = m_sessionStartMs;
    snap.sessionEndMs   = m_sessionEndMs;
    snap.sessionType    = m_sessionType;
    snap.minPrice       = m_minPrice;
    snap.maxPrice       = m_maxPrice;
    snap.tickSize       = m_tickSize;
    snap.gridHeight     = m_gridHeight;
    snap.va             = m_va;
    snap.dirty          = m_dirty;
    snap.generation     = m_generation;
    return snap;
}

void VolumeProfileState::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bins.clear();
    m_sessionStartMs = 0;
    m_sessionEndMs   = 0;
    m_gridHeight     = 0;
    m_va             = {};
    m_dirty          = false;
    ++m_generation;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Steidlmayer 70 % Value-Area Algorithm
//  (static, used by tests independently of instance state)
// ─────────────────────────────────────────────────────────────────────────────
VolumeProfileState::ValueArea
VolumeProfileState::computeValueArea(const std::vector<float>& bins,
                                      double minPrice,
                                      double tickSize,
                                      double vaFraction) {
    ValueArea va;
    const int n = static_cast<int>(bins.size());
    if (n == 0 || tickSize <= 0.0) {
        return va;
    }

    // Step 1 – total volume and POC
    double total = 0.0;
    int    pocRow = 0;
    float  pocVol = bins[0];
    for (int i = 0; i < n; ++i) {
        total += static_cast<double>(bins[static_cast<size_t>(i)]);
        if (bins[static_cast<size_t>(i)] > pocVol) {
            pocVol = bins[static_cast<size_t>(i)];
            pocRow = i;
        }
    }
    if (total <= 0.0) {
        return va;
    }

    va.totalVolume = total;
    va.pocRow      = pocRow;
    // bin[row] centre price: bins are top-to-bottom (row 0 = highest price)
    // centre of row i  = maxPrice - (i + 0.5) * tickSize
    //   where maxPrice = minPrice + n * tickSize
    const double gridMaxPrice = minPrice + static_cast<double>(n) * tickSize;
    va.pocPrice = gridMaxPrice - (static_cast<double>(pocRow) + 0.5) * tickSize;

    // Step 2 – expand VA outward from POC using the Steidlmayer rule:
    //   At each step add the single bin (above or below the current frontier)
    //   with the higher volume.  Repeat until cumulative >= vaFraction * total.
    const double vaTarget = vaFraction * total;
    double       cumVol   = static_cast<double>(bins[static_cast<size_t>(pocRow)]);
    int          hi = pocRow;   // highest row index included (= lowest price)
    int          lo = pocRow;   // lowest  row index included (= highest price)

    while (cumVol < vaTarget) {
        const int nextLo = lo - 1;   // row above = higher price
        const int nextHi = hi + 1;   // row below = lower  price

        const float volAbove = (nextLo >= 0) ? bins[static_cast<size_t>(nextLo)] : -1.0f;
        const float volBelow = (nextHi <  n) ? bins[static_cast<size_t>(nextHi)] : -1.0f;

        if (volAbove < 0.0f && volBelow < 0.0f) {
            break;  // reached both edges
        }

        // Prefer the side with higher volume; break ties by going upward first.
        if (volAbove >= volBelow) {
            lo = nextLo;
            cumVol += static_cast<double>(volAbove);
        } else {
            hi = nextHi;
            cumVol += static_cast<double>(volBelow);
        }
    }

    // lo = lowest row index in VA → highest price (VAH)
    // hi = highest row index in VA → lowest price  (VAL)
    va.vahRow   = lo;
    va.valRow   = hi;
    va.vahPrice = gridMaxPrice - (static_cast<double>(lo) + 0.0) * tickSize;  // top edge of bin
    va.valPrice = gridMaxPrice - (static_cast<double>(hi) + 1.0) * tickSize;  // bottom edge of bin
    va.valid    = true;
    return va;
}
